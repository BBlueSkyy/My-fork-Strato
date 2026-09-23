// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)

#include <soc.h>
#include <soc/gm20b/gmmu.h>
#include <soc/gm20b/channel.h>
#include "gpfifo.h"

namespace skyline::soc::gm20b::engine {
    GPFIFO::GPFIFO(host1x::SyncpointSet &syncpoints, ChannelContext &channelCtx) : syncpoints(syncpoints), channelCtx(channelCtx) {}

    void GPFIFO::CallMethod(u32 method, u32 argument) {
        LOGD("Called method in GPFIFO: 0x{:X} args: 0x{:X}", method, argument);

        registers.raw[method] = argument;

        switch (method) {
            ENGINE_STRUCT_CASE(syncpoint, action, {
                if (action.operation == Registers::Syncpoint::Operation::Incr) {
                    LOGD("Increment syncpoint: {}", +action.index);
                    channelCtx.executor.AddDeferredAction([=, syncpoints = &this->syncpoints, index = action.index]() {
                        syncpoints->at(index).host.Increment();
                    });
                    syncpoints.at(action.index).guest.Increment();
                } else if (action.operation == Registers::Syncpoint::Operation::Wait) {
                    LOGD("Wait syncpoint: {}, thresh: {}", +action.index, registers.syncpoint->payload);
                    LOGI("GRID-WAIT syncpoint-begin id={} threshold={} host={} guest={}",
                         +action.index, registers.syncpoint->payload,
                         syncpoints.at(action.index).host.Load(), syncpoints.at(action.index).guest.Load());

                    // Wait forever for another channel to increment

                    LOGI("GRID-WAIT syncpoint-submit-begin id={}", +action.index);
                    channelCtx.executor.Submit();
                    LOGI("GRID-WAIT syncpoint-submit-end id={}", +action.index);

                    LOGI("GRID-WAIT syncpoint-unlock-begin id={}", +action.index);
                    channelCtx.Unlock();
                    LOGI("GRID-WAIT syncpoint-unlock-end id={}", +action.index);

                    LOGI("GRID-WAIT syncpoint-host-wait-begin id={} threshold={} host={}",
                         +action.index, registers.syncpoint->payload, syncpoints.at(action.index).host.Load());
                    syncpoints.at(action.index).host.Wait(registers.syncpoint->payload, std::chrono::steady_clock::duration::max());
                    LOGI("GRID-WAIT syncpoint-host-wait-end id={} threshold={} host={}",
                         +action.index, registers.syncpoint->payload, syncpoints.at(action.index).host.Load());

                    LOGI("GRID-WAIT syncpoint-relock-begin id={}", +action.index);
                    channelCtx.Lock();
                    LOGI("GRID-WAIT syncpoint-relock-end id={}", +action.index);
                }
            })

            ENGINE_STRUCT_CASE(semaphore, action, {
                u64 address{registers.semaphore->address};

                switch (action.operation) {
                    case Registers::Semaphore::Operation::Acquire: {
                        LOGD("Acquire semaphore: 0x{:X} payload: {}", address, registers.semaphore->payload);
                        LOGI("GRID-WAIT semaphore-eq-begin address=0x{:X} payload={}",
                             address, registers.semaphore->payload);

                        LOGI("GRID-WAIT semaphore-eq-submit-begin address=0x{:X}", address);
                        channelCtx.executor.Submit();
                        LOGI("GRID-WAIT semaphore-eq-submit-end address=0x{:X}", address);

                        LOGI("GRID-WAIT semaphore-eq-unlock-begin address=0x{:X}", address);
                        channelCtx.Unlock();
                        LOGI("GRID-WAIT semaphore-eq-unlock-end address=0x{:X}", address);

                        u32 current{channelCtx.asCtx->gmmu.Read<u32>(address)};
                        LOGI("GRID-WAIT semaphore-eq-spin-begin address=0x{:X} payload={} current={}",
                             address, registers.semaphore->payload, current);
                        while (current != registers.semaphore->payload) {
                            std::this_thread::yield();
                            current = channelCtx.asCtx->gmmu.Read<u32>(address);
                        }
                        LOGI("GRID-WAIT semaphore-eq-spin-end address=0x{:X} payload={} current={}",
                             address, registers.semaphore->payload, current);

                        LOGI("GRID-WAIT semaphore-eq-relock-begin address=0x{:X}", address);
                        channelCtx.Lock();
                        LOGI("GRID-WAIT semaphore-eq-relock-end address=0x{:X}", address);
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
                        break;
                    case Registers::Semaphore::Operation::AcqGeq: {
                        LOGD("Acquire semaphore: 0x{:X} payload: {}", address, registers.semaphore->payload);
                        LOGI("GRID-WAIT semaphore-geq-begin address=0x{:X} payload={}",
                             address, registers.semaphore->payload);

                        LOGI("GRID-WAIT semaphore-geq-submit-begin address=0x{:X}", address);
                        channelCtx.executor.Submit();
                        LOGI("GRID-WAIT semaphore-geq-submit-end address=0x{:X}", address);

                        LOGI("GRID-WAIT semaphore-geq-unlock-begin address=0x{:X}", address);
                        channelCtx.Unlock();
                        LOGI("GRID-WAIT semaphore-geq-unlock-end address=0x{:X}", address);

                        u32 current{channelCtx.asCtx->gmmu.Read<u32>(address)};
                        LOGI("GRID-WAIT semaphore-geq-spin-begin address=0x{:X} payload={} current={}",
                             address, registers.semaphore->payload, current);
                        while (current < registers.semaphore->payload) {
                            std::this_thread::yield();
                            current = channelCtx.asCtx->gmmu.Read<u32>(address);
                        }
                        LOGI("GRID-WAIT semaphore-geq-spin-end address=0x{:X} payload={} current={}",
                             address, registers.semaphore->payload, current);

                        LOGI("GRID-WAIT semaphore-geq-relock-begin address=0x{:X}", address);
                        channelCtx.Lock();
                        LOGI("GRID-WAIT semaphore-geq-relock-end address=0x{:X}", address);
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
