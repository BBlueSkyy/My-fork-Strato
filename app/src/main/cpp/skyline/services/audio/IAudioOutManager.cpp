// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/)

#include <audio_core/audio_out_manager.h>
#include <audio.h>
#include <kernel/types/KProcess.h>
#ifdef __ANDROID__
#include <kernel/scheduler_diagnostics.h>
#include <dlfcn.h>
#include <thread>
#endif
#include "IAudioOutManager.h"
#include "IAudioOut.h"

namespace skyline::service::audio {
    IAudioOutManager::IAudioOutManager(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IAudioOutManager::ListAudioOuts(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::memset(request.outputBuf.at(0).data(), 0, request.outputBuf.at(0).size());
        request.outputBuf.at(0).copy_from(constant::DefaultAudioOutName);
        response.Push<u32>(1); // One audio out
        return {};
    }

    Result IAudioOutManager::OpenAudioOut(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto &inputParams{request.Pop<AudioCore::AudioOut::AudioOutParameter>()};
        auto appletResourceUserId{request.Pop<u64>()};
        std::string_view deviceName{request.inputBuf.at(0).as_string(true)};
        auto handle{request.copyHandles.at(0)};

        auto &audioOutManager{*state.audio->audioOutManager};
        if (auto result{audioOutManager.LinkToManager()}; result.IsError()) {
            LOGW("Failed to link Audio Out to manager");
            return Result{result};
        }

        size_t sessionId{};
        if (auto result{audioOutManager.AcquireSessionId(sessionId)}; result.IsError()) {
            LOGW("Failed to acquire audio session");
            return Result{result};
        }

        auto audioOut{std::make_shared<IAudioOut>(state, manager, sessionId, deviceName, inputParams, handle, appletResourceUserId)};
        manager.RegisterService(audioOut, session, response);

        audioOutManager.sessions[sessionId] = audioOut->impl;
        audioOutManager.applet_resource_user_ids[sessionId] = appletResourceUserId;

        auto &outSystem{audioOut->impl->GetSystem()};
        response.Push<AudioCore::AudioOut::AudioOutParameterInternal>({
            .sample_rate = outSystem.GetSampleRate(),
            .channel_count = outSystem.GetChannelCount(),
            .sample_format = static_cast<u32>(outSystem.GetSampleFormat()),
            .state = static_cast<u32>(outSystem.GetState())
        });

        std::memset(request.outputBuf.at(0).data(), 0, request.outputBuf.at(0).size());

        request.outputBuf.at(0).copy_from(outSystem.GetName());

#ifdef __ANDROID__
        auto diagnosticThread{state.thread};
        if (kernel::Scheduler::DiagnosticReady.load(std::memory_order_acquire) &&
            !diagnosticThread->startupSamplerStarted.exchange(true, std::memory_order_acq_rel)) {
            std::thread([diagnosticThread]() {
                constexpr std::array delays{
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{125},
                    std::chrono::milliseconds{250},
                    std::chrono::milliseconds{250},
                    std::chrono::milliseconds{250},
                    std::chrono::milliseconds{250},
                };
                unsigned round{};
                for (const auto delay : delays) {
                    std::this_thread::sleep_for(delay);
                    ++round;
                    if (!diagnosticThread->RequestStartupSample()) {
                        LOGW("SCHED-DIAG async-audio T{} round={} request-unavailable", diagnosticThread->id, round);
                        continue;
                    }

                    kernel::diagnostic::ContextSample sample{};
                    bool captured{};
                    for (unsigned attempt{}; attempt < 20; ++attempt) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        if (diagnosticThread->ConsumeStartupSample(sample)) {
                            captured = true;
                            break;
                        }
                    }
                    if (!captured) {
                        LOGW("SCHED-DIAG async-audio T{} round={} sample-pending", diagnosticThread->id, round);
                        break;
                    }

                    LOGW("SCHED-DIAG async-audio T{} round={} mode={} pc=0x{:X} lr=0x{:X} fp=0x{:X} sp=0x{:X} x0=0x{:X} x1=0x{:X} x2=0x{:X} x3=0x{:X} x4=0x{:X} x5=0x{:X} x6=0x{:X} x7=0x{:X} x8=0x{:X} x9=0x{:X} x10=0x{:X} x11=0x{:X}",
                         diagnosticThread->id, round, sample.guest ? "guest" : "host", sample.pc,
                         sample.regs[30], sample.regs[29], sample.sp,
                         sample.regs[0], sample.regs[1], sample.regs[2], sample.regs[3],
                         sample.regs[4], sample.regs[5], sample.regs[6], sample.regs[7],
                         sample.regs[8], sample.regs[9], sample.regs[10], sample.regs[11]);

                    if (!sample.guest) {
                        Dl_info pcInfo{}, lrInfo{};
                        const bool pcResolved{dladdr(reinterpret_cast<void *>(sample.pc), &pcInfo) != 0};
                        const bool lrResolved{dladdr(reinterpret_cast<void *>(sample.regs[30]), &lrInfo) != 0};
                        const auto moduleOffset{[](uintptr_t address, const Dl_info &info) {
                            return info.dli_fbase ? address - reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
                        }};
                        const auto symbolOffset{[](uintptr_t address, const Dl_info &info) {
                            return info.dli_saddr ? address - reinterpret_cast<uintptr_t>(info.dli_saddr) : 0;
                        }};
                        LOGW("SCHED-DIAG async-audio-host T{} round={} pcModule={} pcModuleOff=0x{:X} pcSymbol={} pcSymbolOff=0x{:X} lrModule={} lrModuleOff=0x{:X} lrSymbol={} lrSymbolOff=0x{:X}",
                             diagnosticThread->id, round,
                             pcResolved && pcInfo.dli_fname ? pcInfo.dli_fname : "?",
                             pcResolved ? moduleOffset(sample.pc, pcInfo) : 0,
                             pcResolved && pcInfo.dli_sname ? pcInfo.dli_sname : "?",
                             pcResolved ? symbolOffset(sample.pc, pcInfo) : 0,
                             lrResolved && lrInfo.dli_fname ? lrInfo.dli_fname : "?",
                             lrResolved ? moduleOffset(sample.regs[30], lrInfo) : 0,
                             lrResolved && lrInfo.dli_sname ? lrInfo.dli_sname : "?",
                             lrResolved ? symbolOffset(sample.regs[30], lrInfo) : 0);
                    }

                    if (round >= 9) {
                        u64 sequenceBefore{}, sequenceAfter{}, svcPc{}, svcCaller{}, svcArg0{}, svcArg1{};
                        u16 svcId{};
                        for (unsigned attempt{}; attempt < 4; ++attempt) {
                            sequenceBefore = diagnosticThread->startupSvcSequence.load(std::memory_order_acquire);
                            if (sequenceBefore & 1)
                                continue;
                            svcId = diagnosticThread->startupSvcId.load(std::memory_order_relaxed);
                            svcPc = diagnosticThread->startupSvcPc.load(std::memory_order_relaxed);
                            svcCaller = diagnosticThread->startupSvcCaller.load(std::memory_order_relaxed);
                            svcArg0 = diagnosticThread->startupSvcArg0.load(std::memory_order_relaxed);
                            svcArg1 = diagnosticThread->startupSvcArg1.load(std::memory_order_relaxed);
                            sequenceAfter = diagnosticThread->startupSvcSequence.load(std::memory_order_acquire);
                            if (sequenceBefore == sequenceAfter && !(sequenceAfter & 1))
                                break;
                        }
                        LOGW("SCHED-DIAG async-svc T{} round={} events={} svc=0x{:X} pc=0x{:X} caller=0x{:X} x0=0x{:X} x1=0x{:X}",
                             diagnosticThread->id, round, sequenceAfter / 2, svcId, svcPc, svcCaller, svcArg0, svcArg1);
                    }

                    if (round >= 11) {
                        auto front{diagnosticThread->state.scheduler->DiagnosticQueueSnapshot(diagnosticThread, round)};
                        if (front && front != diagnosticThread && front->RequestStartupSample()) {
                            kernel::diagnostic::ContextSample frontSample{};
                            bool frontCaptured{};
                            for (unsigned attempt{}; attempt < 20; ++attempt) {
                                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                if (front->ConsumeStartupSample(frontSample)) {
                                    frontCaptured = true;
                                    break;
                                }
                            }
                            if (frontCaptured) {
                                LOGW("SCHED-DIAG async-front target=T{} round={} front=T{} mode={} pc=0x{:X} lr=0x{:X} fp=0x{:X} sp=0x{:X} x0=0x{:X} x1=0x{:X} x2=0x{:X} x3=0x{:X}",
                                     diagnosticThread->id, round, front->id, frontSample.guest ? "guest" : "host",
                                     frontSample.pc, frontSample.regs[30], frontSample.regs[29], frontSample.sp,
                                     frontSample.regs[0], frontSample.regs[1], frontSample.regs[2], frontSample.regs[3]);
                                if (!frontSample.guest) {
                                    Dl_info frontInfo{};
                                    const bool resolved{dladdr(reinterpret_cast<void *>(frontSample.pc), &frontInfo) != 0};
                                    LOGW("SCHED-DIAG async-front-host target=T{} round={} front=T{} module={} moduleOff=0x{:X} symbol={}",
                                         diagnosticThread->id, round, front->id,
                                         resolved && frontInfo.dli_fname ? frontInfo.dli_fname : "?",
                                         resolved && frontInfo.dli_fbase ? frontSample.pc - reinterpret_cast<uintptr_t>(frontInfo.dli_fbase) : 0,
                                         resolved && frontInfo.dli_sname ? frontInfo.dli_sname : "?");
                                }
                            }
                        }
                    }
                }
            }).detach();
        }
#endif

        return {};
    }
}
