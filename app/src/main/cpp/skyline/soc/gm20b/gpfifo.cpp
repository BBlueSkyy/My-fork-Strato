// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <common/signal.h>
#include <common/settings.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include <soc.h>
#include <os.h>
#include "channel.h"
#include "macro/macro_state.h"

namespace skyline::soc::gm20b {
    /**
     * @brief A single pushbuffer method header that describes a compressed method sequence
     * @url https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/manuals/volta/gv100/dev_ram.ref.txt#L850
     * @url https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/classes/host/clb06f.h#L179
     */
    union PushBufferMethodHeader {
        u32 raw;

        enum class TertOp : u8 {
            Grp0IncMethod = 0,
            Grp0SetSubDevMask = 1,
            Grp0StoreSubDevMask = 2,
            Grp0UseSubDevMask = 3,
            Grp2NonIncMethod = 0,
        };

        enum class SecOp : u8 {
            Grp0UseTert = 0,
            IncMethod = 1,
            Grp2UseTert = 2,
            NonIncMethod = 3,
            ImmdDataMethod = 4,
            OneInc = 5,
            Reserved6 = 6,
            EndPbSegment = 7,
        };

        u16 methodAddress : 12;
        struct {
            u8 _pad0_ : 4;
            u16 subDeviceMask : 12;
        };

        struct {
            u16 _pad1_ : 13;
            SubchannelId methodSubChannel : 3;
            union {
                TertOp tertOp : 3;
                u16 methodCount : 13;
                u16 immdData : 13;
            };
        };

        struct {
            u32 _pad2_ : 29;
            SecOp secOp : 3;
        };

        /**
         * @brief Checks if a method is 'pure' i.e. does not touch macro or GPFIFO methods
         */
        bool Pure() const {
            u32 size{[&]() -> u32  {
                switch (secOp) {
                    case SecOp::NonIncMethod:
                    case SecOp::ImmdDataMethod:
                        return 0;
                    case SecOp::OneInc:
                        return 1;
                    default:
                        return methodCount;
                }
            }()};

            u32 end{static_cast<u32>(methodAddress + size)};
            return end < engine::EngineMethodsEnd && methodAddress >= engine::GPFIFO::RegisterCount;
        }
    };
    static_assert(sizeof(PushBufferMethodHeader) == sizeof(u32));

    ChannelGpfifo::ChannelGpfifo(const DeviceState &state, ChannelContext &channelCtx, size_t numEntries) :
        state(state),
        gpfifoEngine(state.soc->host1x.syncpoints, channelCtx),
        channelCtx(channelCtx),
        gpEntries(numEntries),
        thread(std::thread(&ChannelGpfifo::Run, this)) {}

    void ChannelGpfifo::SendFull(u32 method, GpfifoArgument argument, SubchannelId subChannel, bool lastCall) {
        if (method < engine::GPFIFO::RegisterCount) {
            gpfifoEngine.CallMethod(method, *argument);
        } else if (method < engine::EngineMethodsEnd) { [[likely]]
            SendPure(method, *argument, subChannel);
        } else {
            switch (subChannel) {
                case SubchannelId::ThreeD:
                    skipDirtyFlushes = channelCtx.maxwell3D.HandleMacroCall(method - engine::EngineMethodsEnd, argument, lastCall,
                                                                            [&executor = channelCtx.executor] {
                                                                                executor.Submit({}, true);
                                                                            });
                    break;
                case SubchannelId::TwoD:
                    skipDirtyFlushes = channelCtx.fermi2D.HandleMacroCall(method - engine::EngineMethodsEnd, argument, lastCall,
                                                                          [&executor = channelCtx.executor] {
                                                                              executor.Submit({}, true);
                                                                          });
                    break;
                default:
                    LOGW("Called method 0x{:X} out of bounds for engine 0x{:X}, args: 0x{:X}", method, subChannel, *argument);
                    break;
            }
        }
    }

    void ChannelGpfifo::SendPure(u32 method, u32 argument, SubchannelId subChannel) {
        if (subChannel == SubchannelId::ThreeD) [[likely]] {
            channelCtx.maxwell3D.CallMethod(method, argument);
            return;
        }

        switch (subChannel) {
            case SubchannelId::ThreeD:
                channelCtx.maxwell3D.CallMethod(method, argument);
                break;
            case SubchannelId::Compute:
                channelCtx.keplerCompute.CallMethod(method, argument);
                break;
            case SubchannelId::Inline2Mem:
                channelCtx.inline2Memory.CallMethod(method, argument);
                break;
            case SubchannelId::Copy:
                channelCtx.maxwellDma.CallMethod(method, argument);
                break;
            case SubchannelId::TwoD:
                channelCtx.fermi2D.CallMethod(method, argument);
                break;
            default:
                LOGW("Called method 0x{:X} in unimplemented engine 0x{:X}, args: 0x{:X}", method, subChannel, argument);
                break;
        }
    }

    void ChannelGpfifo::SendPureBatchNonInc(u32 method, span<u32> arguments, SubchannelId subChannel) {
        switch (subChannel) {
            case SubchannelId::ThreeD:
                channelCtx.maxwell3D.CallMethodBatchNonInc(method, arguments);
                break;
            case SubchannelId::Compute:
                channelCtx.keplerCompute.CallMethodBatchNonInc(method, arguments);
                break;
            case SubchannelId::Inline2Mem:
                channelCtx.inline2Memory.CallMethodBatchNonInc(method, arguments);
                break;
            case SubchannelId::Copy:
                channelCtx.maxwellDma.CallMethodBatchNonInc(method, arguments);
                break;
            default:
                LOGW("Called method 0x{:X} in unimplemented engine 0x{:X} with batch args", method, subChannel);
                break;
        }
    }

    void ChannelGpfifo::Process(GpEntry gpEntry) {
        if (!gpEntry.size) {
            // This is a GPFIFO control entry, all control entries have a zero length and contain no pushbuffers
            switch (gpEntry.opcode) {
                case GpEntry::Opcode::Nop:
                    return;
                default:
                    LOGW("Unsupported GpEntry control opcode used: {}", static_cast<u8>(gpEntry.opcode));
                    return;
            }
        }

        const bool gridDeepTrace{gpEntry.Address() == 0x502032420 && gpEntry.size == 0x13};
        if (gridDeepTrace)
            LOGI("GRID-DEEP translate-range-begin address=0x{:X} size=0x{:X} bytes=0x{:X}",
                 gpEntry.Address(), +gpEntry.size, gpEntry.size * sizeof(u32));

        auto pushBufferMappedRanges{channelCtx.asCtx->gmmu.TranslateRange(gpEntry.Address(), gpEntry.size * sizeof(u32))};

        if (gridDeepTrace)
            LOGI("GRID-DEEP translate-range-end ranges={}", pushBufferMappedRanges.size());

        bool pushBufferCopied{}; //!< Set by the below lambda in order to track if the pushbuffer is a copy of guest memory or not
        auto pushBuffer{[&]() -> span<u32> {
            if (pushBufferMappedRanges.size() == 1) {
                return pushBufferMappedRanges.front().cast<u32>();
            } else {
                // Create an intermediate copy of pushbuffer data if it's split across multiple mappings
                pushBufferData.resize(gpEntry.size);
                channelCtx.asCtx->gmmu.Read<u32>(pushBufferData, gpEntry.Address());
                pushBufferCopied = true;
                return span(pushBufferData);
            }
        }()};

        if (gridDeepTrace)
            LOGI("GRID-DEEP pushbuffer-ready copied={} words={}", pushBufferCopied, pushBuffer.size());

        bool pushbufferDirty{false};
        size_t dirtyRangeIndex{};

        for (auto range : pushBufferMappedRanges) {
            bool intersectsDirty{channelCtx.executor.usageTracker.dirtyIntervals.Intersect(range)};
            if (gridDeepTrace)
                LOGI("GRID-DEEP dirty-check index={} intersects={} skipDirtyFlushes={}",
                     dirtyRangeIndex, intersectsDirty, skipDirtyFlushes);

            if (intersectsDirty) {
                if (skipDirtyFlushes) {
                    pushbufferDirty = true;
                    if (gridDeepTrace)
                        LOGI("GRID-DEEP dirty-mark-buffer index={}", dirtyRangeIndex);
                } else {
                    if (gridDeepTrace)
                        LOGI("GRID-DEEP dirty-submit-begin index={}", dirtyRangeIndex);
                    channelCtx.executor.Submit({}, true);
                    if (gridDeepTrace)
                        LOGI("GRID-DEEP dirty-submit-end index={}", dirtyRangeIndex);
                }
            }

            dirtyRangeIndex++;
        }

        if (gridDeepTrace)
            LOGI("GRID-DEEP dirty-scan-end pushbufferDirty={}", pushbufferDirty);

        // There will be at least one entry here
        auto entry{pushBuffer.begin()};

        auto getArgument{[&](){
            return GpfifoArgument{pushBufferCopied ? *entry : 0, pushBufferCopied ? nullptr : entry.base(), pushbufferDirty};
        }};

        // Executes the current split method, returning once execution is finished or the current GpEntry has reached its end
        auto resumeSplitMethod{[&](){
            switch (resumeState.state) {
                case MethodResumeState::State::Inc:
                    while (entry != pushBuffer.end() && resumeState.remaining) {
                        SendFull(resumeState.address++, getArgument(), resumeState.subChannel, --resumeState.remaining == 0);
                        entry++;
                    }

                    break;
                case MethodResumeState::State::OneInc:
                    SendFull(resumeState.address++, getArgument(), resumeState.subChannel, --resumeState.remaining == 0);
                    entry++;

                    // After the first increment OneInc methods work the same as a NonInc method, this is needed so they can resume correctly if they are broken up by multiple GpEntries
                    resumeState.state = MethodResumeState::State::NonInc;
                    [[fallthrough]];
                case MethodResumeState::State::NonInc:
                    while (entry != pushBuffer.end() && resumeState.remaining) {
                        SendFull(resumeState.address, getArgument(), resumeState.subChannel, --resumeState.remaining == 0);
                        entry++;
                    }

                    break;
            }
        }};

        // We've a method from a previous GpEntry that needs resuming
        if (resumeState.remaining) {
            if (gridDeepTrace)
                LOGI("GRID-DEEP resume-begin remaining={} address=0x{:X} subchannel={} state={}",
                     resumeState.remaining, resumeState.address, static_cast<u32>(resumeState.subChannel),
                     static_cast<u32>(resumeState.state));
            resumeSplitMethod();
            if (gridDeepTrace)
                LOGI("GRID-DEEP resume-end remaining={} wordIndex={}",
                     resumeState.remaining, std::distance(pushBuffer.begin(), entry));
        }

        // Process more methods if the entries are still not all used up after handling resuming
        for (; entry != pushBuffer.end(); entry++) {
            if (entry >= pushBuffer.end()) [[unlikely]]
                throw exception("GPFIFO buffer overflow!"); // This should never happen

            // Entries containing all zeroes is a NOP, skip over them
            for (; *entry == 0; entry++)
                if (entry == std::prev(pushBuffer.end()))
                    return;

            PushBufferMethodHeader methodHeader{.raw = *entry};

            // Needed in order to check for methods split across multiple GpEntries
            ssize_t remainingEntries{std::distance(entry, pushBuffer.end()) - 1};

            if (gridDeepTrace)
                LOGI("GRID-DEEP method-header wordIndex={} raw=0x{:08X} secOp={} method=0x{:X} subchannel={} count={} remainingEntries={} pure={}",
                     std::distance(pushBuffer.begin(), entry), methodHeader.raw,
                     static_cast<u32>(methodHeader.secOp), +methodHeader.methodAddress,
                     static_cast<u32>(methodHeader.methodSubChannel), +methodHeader.methodCount,
                     remainingEntries, methodHeader.Pure());

            // Handles storing state and initial execution for methods that are split across multiple GpEntries
            auto startSplitMethod{[&](auto methodState) {
                resumeState = {
                    .remaining = methodHeader.methodCount,
                    .address = methodHeader.methodAddress,
                    .subChannel = methodHeader.methodSubChannel,
                    .state = methodState
                };

                // Skip over method header as `resumeSplitMethod` doesn't expect it to be there
                entry++;

                if (gridDeepTrace)
                    LOGI("GRID-DEEP split-resume-begin remaining={} address=0x{:X} subchannel={} state={}",
                         resumeState.remaining, resumeState.address, static_cast<u32>(resumeState.subChannel),
                         static_cast<u32>(resumeState.state));
                resumeSplitMethod();
                if (gridDeepTrace)
                    LOGI("GRID-DEEP split-resume-end remaining={} wordIndex={}",
                         resumeState.remaining, std::distance(pushBuffer.begin(), entry));
            }};

            /**
             * @brief Handles execution of a specific method type as specified by the State template parameter
             */
            auto dispatchCalls{[&]<MethodResumeState::State State> () {
                /**
                 * @brief Gets the offset to apply to the method address for a given dispatch loop index
                 */
                auto methodOffset{[] (u32 i) -> u32 {
                    if constexpr(State == MethodResumeState::State::Inc)
                        return i;
                    else if constexpr (State == MethodResumeState::State::OneInc)
                        return i ? 1 : 0;
                    else
                        return 0;
                }};

                constexpr u32 BatchCutoff{4}; //!< Cutoff needed to send method calls in a batch which is espcially important for UBO updates. This helps to avoid the extra overhead batching for small packets.
                // TODO: Only batch for specific target methods like UBO updates, since normal dispatch is generally cheaper

                if (remainingEntries >= methodHeader.methodCount) { [[likely]]
                    if (methodHeader.Pure()) [[likely]] {
                        if constexpr (State == MethodResumeState::State::NonInc) {
                            // For pure noninc methods we can send all method calls as a span in one go
                            if (methodHeader.methodCount > BatchCutoff) [[unlikely]] {
                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP pure-batch-begin method=0x{:X} count={} subchannel={}",
                                         +methodHeader.methodAddress, +methodHeader.methodCount,
                                         static_cast<u32>(methodHeader.methodSubChannel));
                                SendPureBatchNonInc(methodHeader.methodAddress, span(&(*++entry), methodHeader.methodCount), methodHeader.methodSubChannel);
                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP pure-batch-end method=0x{:X}", +methodHeader.methodAddress);

                                entry += methodHeader.methodCount - 1;
                                return false;
                            }
                        } else if constexpr (State == MethodResumeState::State::OneInc) {
                            // For pure oneinc methods we can send the initial method then send the rest as a span in one go
                            if (methodHeader.methodCount > (BatchCutoff + 1)) [[unlikely]] {
                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP oneinc-first-begin method=0x{:X} subchannel={}",
                                         +methodHeader.methodAddress, static_cast<u32>(methodHeader.methodSubChannel));
                                SendPure(methodHeader.methodAddress, *++entry, methodHeader.methodSubChannel);
                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP oneinc-first-end method=0x{:X}", +methodHeader.methodAddress);

                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP oneinc-batch-begin method=0x{:X} count={} subchannel={}",
                                         +methodHeader.methodAddress + 1, +methodHeader.methodCount - 1,
                                         static_cast<u32>(methodHeader.methodSubChannel));
                                SendPureBatchNonInc(methodHeader.methodAddress + 1, span((++entry).base(), methodHeader.methodCount - 1), methodHeader.methodSubChannel);
                                if (gridDeepTrace)
                                    LOGI("GRID-DEEP oneinc-batch-end method=0x{:X}", +methodHeader.methodAddress + 1);

                                entry += methodHeader.methodCount - 2;
                                return false;
                            }
                        }

                        #pragma unroll(2)
                        for (u32 i{}; i < methodHeader.methodCount; i++) {
                            u32 dispatchedMethod{static_cast<u32>(methodHeader.methodAddress + methodOffset(i))};
                            u32 argument{*++entry};
                            if (gridDeepTrace)
                                LOGI("GRID-DEEP send-pure-begin index={} method=0x{:X} arg=0x{:08X} subchannel={}",
                                     i, dispatchedMethod, argument, static_cast<u32>(methodHeader.methodSubChannel));
                            SendPure(dispatchedMethod, argument, methodHeader.methodSubChannel);
                            if (gridDeepTrace)
                                LOGI("GRID-DEEP send-pure-end index={} method=0x{:X}", i, dispatchedMethod);
                        }
                    } else {
                        // Slow path for methods that touch GPFIFO or macros
                        for (u32 i{}; i < methodHeader.methodCount; i++) {
                            entry++;
                            u32 dispatchedMethod{static_cast<u32>(methodHeader.methodAddress + methodOffset(i))};
                            if (gridDeepTrace)
                                LOGI("GRID-DEEP send-full-begin index={} method=0x{:X} arg=0x{:08X} subchannel={} last={}",
                                     i, dispatchedMethod, *entry, static_cast<u32>(methodHeader.methodSubChannel),
                                     i == methodHeader.methodCount - 1);
                            SendFull(dispatchedMethod, getArgument(), methodHeader.methodSubChannel, i == methodHeader.methodCount - 1);
                            if (gridDeepTrace)
                                LOGI("GRID-DEEP send-full-end index={} method=0x{:X}", i, dispatchedMethod);
                        }
                    }
                } else {
                    startSplitMethod(State);
                    return true;
                }

                return false;
            }};

            /**
             * @brief Handles execution of a single method
             * @return If the this was the final method in the current GpEntry
             */
            auto processMethod{[&] () -> bool {
                if (methodHeader.secOp == PushBufferMethodHeader::SecOp::IncMethod)  [[likely]] {
                    return dispatchCalls.operator()<MethodResumeState::State::Inc>();
                } else if (methodHeader.secOp == PushBufferMethodHeader::SecOp::OneInc) [[likely]] {
                    return dispatchCalls.operator()<MethodResumeState::State::OneInc>();
                } else if (methodHeader.secOp == PushBufferMethodHeader::SecOp::ImmdDataMethod) {
                    if (methodHeader.Pure()) {
                        if (gridDeepTrace)
                            LOGI("GRID-DEEP immd-pure-begin method=0x{:X} arg=0x{:X} subchannel={}",
                                 +methodHeader.methodAddress, +methodHeader.immdData,
                                 static_cast<u32>(methodHeader.methodSubChannel));
                        SendPure(methodHeader.methodAddress, methodHeader.immdData, methodHeader.methodSubChannel);
                        if (gridDeepTrace)
                            LOGI("GRID-DEEP immd-pure-end method=0x{:X}", +methodHeader.methodAddress);
                    } else {
                        if (gridDeepTrace)
                            LOGI("GRID-DEEP immd-full-begin method=0x{:X} arg=0x{:X} subchannel={}",
                                 +methodHeader.methodAddress, +methodHeader.immdData,
                                 static_cast<u32>(methodHeader.methodSubChannel));
                        SendFull(methodHeader.methodAddress, GpfifoArgument{methodHeader.immdData}, methodHeader.methodSubChannel, true);
                        if (gridDeepTrace)
                            LOGI("GRID-DEEP immd-full-end method=0x{:X}", +methodHeader.methodAddress);
                    }

                    return false;
                } else if (methodHeader.secOp == PushBufferMethodHeader::SecOp::NonIncMethod) [[unlikely]] {
                    return dispatchCalls.operator()<MethodResumeState::State::NonInc>();
                } else if (methodHeader.secOp == PushBufferMethodHeader::SecOp::EndPbSegment) [[unlikely]] {
                    return true;
                } else if (methodHeader.secOp == PushBufferMethodHeader::SecOp::Grp0UseTert) {
                    if (methodHeader.tertOp == PushBufferMethodHeader::TertOp::Grp0SetSubDevMask)
                        return false;

                    throw exception("Unsupported pushbuffer method TertOp: {}", static_cast<u8>(methodHeader.tertOp));
                } else {
                    throw exception("Unsupported pushbuffer method SecOp: {}", static_cast<u8>(methodHeader.secOp));
                }
            }};

            bool hitEnd{[&]() {
                if (methodHeader.methodSubChannel != SubchannelId::ThreeD) [[unlikely]] {
                    if (gridDeepTrace)
                        LOGI("GRID-DEEP flush-engine-state-begin subchannel={}",
                             static_cast<u32>(methodHeader.methodSubChannel));
                    channelCtx.maxwell3D.FlushEngineState(); // Flush the 3D engine state when doing any calls to other engines
                    if (gridDeepTrace)
                        LOGI("GRID-DEEP flush-engine-state-end subchannel={}",
                             static_cast<u32>(methodHeader.methodSubChannel));
                }

                if (gridDeepTrace)
                    LOGI("GRID-DEEP process-method-begin secOp={} method=0x{:X}",
                         static_cast<u32>(methodHeader.secOp), +methodHeader.methodAddress);
                bool result{processMethod()};
                if (gridDeepTrace)
                    LOGI("GRID-DEEP process-method-end secOp={} method=0x{:X} hitEnd={}",
                         static_cast<u32>(methodHeader.secOp), +methodHeader.methodAddress, result);
                return result;
            }()};

            if (hitEnd)
                break;
        }
    }

    void ChannelGpfifo::Run() {
        if (int result{pthread_setname_np(pthread_self(), "GPFIFO")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();
        LOGI("GRID-GPFIFO run-start");

        try {
            bool channelLocked{};

            gpEntries.Process([this, &channelLocked](GpEntry gpEntry) {
                LOGI("GRID-POST consumer-received address=0x{:X} size=0x{:X}",
                     gpEntry.Address(), +gpEntry.size);
                LOGD("Processing pushbuffer: 0x{:X}, Size: 0x{:X}", gpEntry.Address(), +gpEntry.size);

                if (!channelLocked) {
                    LOGI("GRID-POST channel-lock-begin");
                    channelCtx.Lock();
                    LOGI("GRID-POST channel-lock-end");
                    channelLocked = true;
                }

                LOGI("GRID-POST process-begin address=0x{:X} size=0x{:X}",
                     gpEntry.Address(), +gpEntry.size);
                Process(gpEntry);
                LOGI("GRID-POST process-end address=0x{:X} size=0x{:X}",
                     gpEntry.Address(), +gpEntry.size);
                LOGI("GRID-GPFIFO callback-return address=0x{:X} size=0x{:X}",
                     gpEntry.Address(), +gpEntry.size);
            }, [this, &channelLocked]() {
                // If we run out of GpEntries to process ensure we submit any remaining GPU work before waiting for more to arrive
                LOGD("Finished processing pushbuffer batch");
                LOGI("GRID-POST batch-drained channelLocked={}", channelLocked);
                if (channelLocked) {
                    LOGI("GRID-POST executor-submit-begin");
                    channelCtx.executor.Submit();
                    LOGI("GRID-POST executor-submit-end");
                    LOGI("GRID-POST channel-unlock-begin");
                    channelCtx.Unlock();
                    LOGI("GRID-POST channel-unlock-end");
                    channelLocked = false;
                }
            });
        } catch (const signal::SignalException &e) {
            LOGI("GRID-GPFIFO signal-exception signal={} isSigint={}", e.signal, e.signal == SIGINT);
            if (e.signal != SIGINT) {
                LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
        } catch (const exception &e) {
            LOGI("GRID-GPFIFO exception type=skyline what={}", e.what());
            LOGENF("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        } catch (const std::exception &e) {
            LOGI("GRID-GPFIFO exception type=std what={}", e.what());
            LOGE("{}", e.what());
            signal::BlockSignal({SIGINT});
            state.process->Kill(false);
        }

        LOGI("GRID-GPFIFO run-exit");
    }

    void ChannelGpfifo::Push(span<GpEntry> entries) {
        gpEntries.Append(entries);
    }

    void ChannelGpfifo::Push(GpEntry entry) {
        gpEntries.Push(entry);
    }

    ChannelGpfifo::~ChannelGpfifo() {
        LOGI("GRID-LIFE ChannelGpfifo dtor-begin this={} joinable={}",
             static_cast<const void *>(this), thread.joinable());
        if (thread.joinable()) {
            LOGI("GRID-LIFE ChannelGpfifo close-begin this={}", static_cast<const void *>(this));
            gpEntries.Close();
            LOGI("GRID-LIFE ChannelGpfifo close-end this={}", static_cast<const void *>(this));
            LOGI("GRID-LIFE ChannelGpfifo join-begin this={}", static_cast<const void *>(this));
            thread.join();
            LOGI("GRID-LIFE ChannelGpfifo join-end this={}", static_cast<const void *>(this));
        }
        LOGI("GRID-LIFE ChannelGpfifo dtor-end this={}", static_cast<const void *>(this));
    }
}
