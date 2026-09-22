// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)

#include <soc.h>
#include <soc/gm20b/gmmu.h>
#include <soc/gm20b/channel.h>
#include <xv2_trace.h>
#include "gpfifo.h"

namespace skyline::soc::gm20b::engine {
    GPFIFO::GPFIFO(host1x::SyncpointSet &syncpoints, ChannelContext &channelCtx) : syncpoints(syncpoints), channelCtx(channelCtx) {}

    void GPFIFO::CallMethod(u32 method, u32 argument) {
        LOGD("Called method in GPFIFO: 0x{:X} args: 0x{:X}", method, argument);

        u32 traceSeq{256};
        if (diagnostics::xv2::PostCloseActive()) {
            traceSeq = diagnostics::xv2::NextPullerSequence();
            if (traceSeq < 256)
                LOGI("XV2-GPFIFO method epoch={} seq={} method=0x{:X} arg=0x{:X}",
                     diagnostics::xv2::Epoch(), traceSeq, method, argument);
        }

        registers.raw[method] = argument;

        switch (method) {
            ENGINE_STRUCT_CASE(syncpoint, action, {
                auto &syncpoint{syncpoints.at(action.index)};
                auto payload{registers.syncpoint->payload};
                if (action.operation == Registers::Syncpoint::Operation::Incr) {
                    auto guestBefore{syncpoint.guest.Load()};
                    auto hostBefore{syncpoint.host.Load()};
                    if (traceSeq < 256)
                        LOGI("XV2-GPFIFO syncpoint-incr epoch={} seq={} id={} payload={} guest-before={} host-before={}",
                             diagnostics::xv2::Epoch(), traceSeq, +action.index, payload, guestBefore, hostBefore);

                    channelCtx.executor.AddDeferredAction([=, syncpoints = &this->syncpoints, index = action.index]() {
                        auto &deferredSyncpoint{syncpoints->at(index)};
                        auto before{deferredSyncpoint.host.Load()};
                        auto after{deferredSyncpoint.host.Increment()};
                        if (diagnostics::xv2::PostCloseActive())
                            LOGI("XV2-GPFIFO syncpoint-incr-host epoch={} id={} host-before={} host-after={}",
                                 diagnostics::xv2::Epoch(), +index, before, after);
                    });
                    auto guestAfter{syncpoint.guest.Increment()};
                    if (traceSeq < 256)
                        LOGI("XV2-GPFIFO syncpoint-incr-guest epoch={} seq={} id={} guest-after={} host-now={}",
                             diagnostics::xv2::Epoch(), traceSeq, +action.index, guestAfter, syncpoint.host.Load());
                } else if (action.operation == Registers::Syncpoint::Operation::Wait) {
                    auto guestBefore{syncpoint.guest.Load()};
                    auto hostBefore{syncpoint.host.Load()};
                    LOGI("XV2-GPFIFO-BLOCK syncpoint-wait epoch={} id={} threshold={} wait-switch={} guest={} host={}",
                         diagnostics::xv2::Epoch(), +action.index, payload,
                         static_cast<u32>(action.waitSwitch), guestBefore, hostBefore);

                    // Wait forever for another channel to increment
                    channelCtx.executor.Submit();
                    channelCtx.Unlock();

                    LOGI("XV2-GPFIFO-BLOCK wait-enter epoch={} id={} threshold={} guest={} host={}",
                         diagnostics::xv2::Epoch(), +action.index, payload,
                         syncpoint.guest.Load(), syncpoint.host.Load());

                    bool waitResult{syncpoint.host.Wait(payload, std::chrono::steady_clock::duration::max())};

                    LOGI("XV2-GPFIFO-BLOCK wait-exit epoch={} id={} threshold={} result={} guest={} host={}",
                         diagnostics::xv2::Epoch(), +action.index, payload, waitResult,
                         syncpoint.guest.Load(), syncpoint.host.Load());

                    channelCtx.Lock();
                }
            })

            ENGINE_STRUCT_CASE(semaphore, action, {
                u64 address{registers.semaphore->address};

                switch (action.operation) {
                    case Registers::Semaphore::Operation::Acquire: {
                        auto payload{registers.semaphore->payload};
                        auto initial{channelCtx.asCtx->gmmu.Read<u32>(address)};
                        LOGI("XV2-GPFIFO-BLOCK semaphore-acquire epoch={} address=0x{:X} payload={} initial={}",
                             diagnostics::xv2::Epoch(), address, payload, initial);
                        channelCtx.executor.Submit();
                        channelCtx.Unlock();

                        LOGI("XV2-GPFIFO-BLOCK semaphore-acquire-enter epoch={} address=0x{:X}",
                             diagnostics::xv2::Epoch(), address);

                        while (channelCtx.asCtx->gmmu.Read<u32>(address) != payload)
                            std::this_thread::yield();

                        LOGI("XV2-GPFIFO-BLOCK semaphore-acquire-exit epoch={} address=0x{:X} value={}",
                             diagnostics::xv2::Epoch(), address, channelCtx.asCtx->gmmu.Read<u32>(address));

                        channelCtx.Lock();
                        break;
                    }
                    case Registers::Semaphore::Operation::Release:
                        channelCtx.executor.AddDeferredAction([this, action, address, payload = registers.semaphore->payload] () {
                            // Write timestamp first to ensure ordering
                            if (action.releaseSize == Registers::Semaphore::ReleaseSize::SixteenBytes) {
                                channelCtx.asCtx->gmmu.Write<u32>(address + 4, 0);
                                channelCtx.asCtx->gmmu.Write(address + 8, GetGpuTimeTicks());
                            }

                            channelCtx.asCtx->gmmu.Write(address, payload);
                        });

                        LOGD("SemaphoreRelease: address: 0x{:X} payload: {}", address, registers.semaphore->payload);
                        if (traceSeq < 256)
                            LOGI("XV2-GPFIFO semaphore-release epoch={} seq={} address=0x{:X} payload={} release-size={}",
                                 diagnostics::xv2::Epoch(), traceSeq, address, registers.semaphore->payload,
                                 static_cast<u32>(action.releaseSize));
                        break;
                    case Registers::Semaphore::Operation::AcqGeq: {
                        auto payload{registers.semaphore->payload};
                        auto initial{channelCtx.asCtx->gmmu.Read<u32>(address)};
                        LOGI("XV2-GPFIFO-BLOCK semaphore-acqgeq epoch={} address=0x{:X} payload={} initial={}",
                             diagnostics::xv2::Epoch(), address, payload, initial);
                        channelCtx.executor.Submit();
                        channelCtx.Unlock();

                        LOGI("XV2-GPFIFO-BLOCK semaphore-acqgeq-enter epoch={} address=0x{:X}",
                             diagnostics::xv2::Epoch(), address);

                        while (channelCtx.asCtx->gmmu.Read<u32>(address) < payload)
                            std::this_thread::yield();

                        LOGI("XV2-GPFIFO-BLOCK semaphore-acqgeq-exit epoch={} address=0x{:X} value={}",
                             diagnostics::xv2::Epoch(), address, channelCtx.asCtx->gmmu.Read<u32>(address));

                        channelCtx.Lock();
                        break;
                    }
                    case Registers::Semaphore::Operation::Reduction: {
                        u32 origVal{channelCtx.asCtx->gmmu.Read<u32>(address)};
                        bool isSigned{action.format == Registers::Semaphore::Format::Signed};

                        // https://github.com/NVIDIA/open-gpu-doc/blob/b7d1bd16fe62135ebaec306b39dfdbd9e5657827/manuals/turing/tu104/dev_pbdma.ref.txt#L3549
                        u32 val{[](Registers::Semaphore::Reduction reduction, u32 origVal, u32 payload, bool isSigned) {
                            switch (reduction) {
                                case Registers::Semaphore::Reduction::Min:
                                    if (isSigned)
                                        return static_cast<u32>(std::min(static_cast<i32>(origVal), static_cast<i32>(payload)));
                                    else
                                        return std::min(origVal, payload);
                                case Registers::Semaphore::Reduction::Max:
                                    if (isSigned)
                                        return static_cast<u32>(std::max(static_cast<i32>(origVal), static_cast<i32>(payload)));
                                    else
                                        return std::max(origVal, payload);
                                case Registers::Semaphore::Reduction::Xor:
                                    return origVal ^ payload;
                                case Registers::Semaphore::Reduction::And:
                                    return origVal & payload;
                                case Registers::Semaphore::Reduction::Or:
                                    return origVal | payload;
                                case Registers::Semaphore::Reduction::Add:
                                    if (isSigned)
                                        return static_cast<u32>(static_cast<i32>(origVal) + static_cast<i32>(payload));
                                    else
                                        return origVal + payload;
                                case Registers::Semaphore::Reduction::Inc:
                                    return (origVal >= payload) ? 0 : origVal + 1;
                                case Registers::Semaphore::Reduction::Dec:
                                    return (origVal == 0 || origVal > payload) ? payload : origVal - 1;
                            }
                        }(registers.semaphore->action.reduction, origVal, registers.semaphore->payload, isSigned)};
                        LOGD("SemaphoreReduction: address: 0x{:X} op: {} payload: {} original value: {} reduced value: {}",
                                      address, static_cast<u8>(registers.semaphore->action.reduction), registers.semaphore->payload, origVal, val);

                        channelCtx.asCtx->gmmu.Write(address, val);
                        break;
                    }
                    default:
                        LOGW("Unimplemented semaphore operation: 0x{:X}", static_cast<u8>(registers.semaphore->action.operation));
                        break;
                }
            })
            ENGINE_CASE(wfi, {
                channelCtx.executor.AddFullBarrier();
            })
            ENGINE_CASE(setReference, {
                channelCtx.executor.AddFullBarrier();
            })
        }
    };
}
