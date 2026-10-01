// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include <adrenotools/driver.h>
#include <gpu.h>
#include <kernel/memory.h>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include <common/settings.h>
#include "buffer.h"

namespace skyline::gpu {
    namespace {
        constexpr u64 BufferReadbackDiagLogInterval{256};

        std::atomic<u64> bufferReadbackDiagEvents{};
        std::atomic<u64> bufferFastWriteHits{};
        std::atomic<u64> bufferReadPreciseFallbacks{};
        std::atomic<u64> bufferWritePreciseFallbacks{};
        std::atomic<u64> bufferPreciseSyncs{};
        std::atomic<u64> bufferPreciseSyncNs{};
        std::atomic<u64> bufferPreTrapWaits{};
        std::atomic<u64> bufferPreTrapWaitNs{};
        std::atomic<u64> bufferReadOnlyCycleBypasses{};

        void LogBufferReadbackDiag() {
            const auto events{bufferReadbackDiagEvents.fetch_add(1, std::memory_order_relaxed) + 1};
            if (events != 1 && (events % BufferReadbackDiagLogInterval) != 0)
                return;

            LOGI("[FastReadbackDiag][Buffer] events={} fast_write_hits={} read_precise={} write_precise={} precise_syncs={} precise_sync_us={} pretrap_waits={} pretrap_wait_us={} read_only_cycle_bypasses={}",
                 events,
                 bufferFastWriteHits.load(std::memory_order_relaxed),
                 bufferReadPreciseFallbacks.load(std::memory_order_relaxed),
                 bufferWritePreciseFallbacks.load(std::memory_order_relaxed),
                 bufferPreciseSyncs.load(std::memory_order_relaxed),
                 bufferPreciseSyncNs.load(std::memory_order_relaxed) / 1000,
                 bufferPreTrapWaits.load(std::memory_order_relaxed),
                 bufferPreTrapWaitNs.load(std::memory_order_relaxed) / 1000,
                 bufferReadOnlyCycleBypasses.load(std::memory_order_relaxed));
        }
    }

    void Buffer::ResetMegabufferState() {
        if (megaBufferTableUsed)
            megaBufferTableValidity.reset();

        megaBufferTableUsed = false;
        megaBufferViewAccumulatedSize = 0;
        unifiedMegaBuffer = {};
    }

    void Buffer::SetupStagedTraps() {
        if (isDirect)
            return;

        // We can't just capture this in the lambda since the lambda could exceed the lifetime of the buffer
        std::weak_ptr<Buffer> weakThis{shared_from_this()};
        trapHandle = gpu.state.process->trap.CreateTrap(*guest, [weakThis](u8 *faultAddress, bool write) {
            auto buffer{weakThis.lock()};
            if (!buffer)
                return;

            std::unique_lock stateLock{buffer->stateMutex};
            if (buffer->AllCpuBackingWritesBlocked() || buffer->dirtyState == DirtyState::GpuDirty) {
                stateLock.unlock(); // If the lock isn't unlocked, a deadlock from threads waiting on the other lock can occur

                // If this mutex would cause other callbacks to be blocked then we should block on this mutex in advance
                std::shared_ptr<FenceCycle> waitCycle{};
                do {
                    i64 waitNs{};
                    if (waitCycle) {
                        i64 startNs{util::GetTimeNs()};
                        waitCycle->Wait();
                        waitNs = util::GetTimeNs() - startNs;

                        bufferPreTrapWaits.fetch_add(1, std::memory_order_relaxed);
                        bufferPreTrapWaitNs.fetch_add(static_cast<u64>(waitNs), std::memory_order_relaxed);
                        LogBufferReadbackDiag();
                    }

                    std::scoped_lock lock{*buffer};
                    if (waitCycle) {
                        if (buffer->accumulatedGuestWaitCounter > FastReadbackHackWaitCountThreshold)
                            buffer->accumulatedGuestWaitTime += std::chrono::nanoseconds(waitNs);
                        buffer->accumulatedGuestWaitCounter++;

                        if (buffer->writeCycle == waitCycle) {
                            buffer->writeCycle = {};
                            buffer->writeCycleStorageWriteRanges.Clear();
                            buffer->writeCycleStorageRangesComplete = false;
                        }
                        if (buffer->cycle == waitCycle)
                            buffer->cycle = {};
                    }

                    bool useWriteCycle{write && buffer->CanUseFastWriteReadback()};
                    auto nextCycle{useWriteCycle ? buffer->writeCycle : buffer->cycle};
                    if (useWriteCycle && buffer->cycle && buffer->cycle != nextCycle) {
                        bufferReadOnlyCycleBypasses.fetch_add(1, std::memory_order_relaxed);
                        buffer->fastWriteReadbackBypasses++;
                    }

                    if (useWriteCycle && nextCycle && nextCycle == buffer->writeCycle) {
                        bool classified{};
                        if (buffer->writeCycleStorageRangesComplete && buffer->guest) {
                            const uintptr_t pageStart{reinterpret_cast<uintptr_t>(faultAddress) & ~(constant::PageSize - 1)};
                            const uintptr_t pageEnd{pageStart + constant::PageSize};
                            const uintptr_t guestStart{reinterpret_cast<uintptr_t>(buffer->guest->data())};
                            const uintptr_t guestEnd{guestStart + buffer->guest->size()};
                            const uintptr_t overlapStart{std::max(pageStart, guestStart)};
                            const uintptr_t overlapEnd{std::min(pageEnd, guestEnd)};

                            if (overlapStart < overlapEnd) {
                                IntervalList<size_t>::Interval faultRange{
                                    overlapStart - guestStart,
                                    overlapEnd - guestStart,
                                };
                                if (buffer->writeCycleStorageWriteRanges.Intersect(faultRange))
                                    buffer->writeFaultWriterRangeOverlap++;
                                else
                                    buffer->writeFaultWriterRangeDisjoint++;
                                classified = true;
                            }
                        }

                        if (!classified)
                            buffer->writeFaultWriterRangeUnknown++;
                    }

                    waitCycle = std::move(nextCycle);
                } while (waitCycle);
            }
        }, [weakThis] {
            TRACE_EVENT("gpu", "Buffer::ReadTrap");

            auto buffer{weakThis.lock()};
            if (!buffer)
                return true;

            std::unique_lock stateLock{buffer->stateMutex, std::try_to_lock};
            if (!stateLock)
                return false;

            if (buffer->dirtyState != DirtyState::GpuDirty)
                return true; // If state is already CPU dirty/Clean we don't need to do anything

            std::unique_lock lock{*buffer, std::try_to_lock};
            if (!lock)
                return false;

            if (buffer->cycle)
                return false;

            bufferReadPreciseFallbacks.fetch_add(1, std::memory_order_relaxed);
            LogBufferReadbackDiag();
            buffer->SynchronizeGuest(true); // We can skip trapping since the caller will do it
            return true;
        }, [weakThis] {
            TRACE_EVENT("gpu", "Buffer::WriteTrap");

            auto buffer{weakThis.lock()};
            if (!buffer)
                return true;

            std::unique_lock stateLock{buffer->stateMutex, std::try_to_lock};
            if (!stateLock)
                return false;

            if (!buffer->AllCpuBackingWritesBlocked() && buffer->dirtyState != DirtyState::GpuDirty) {
                buffer->dirtyState = DirtyState::CpuDirty;
                return true;
            }

            std::unique_lock lock{*buffer, std::try_to_lock};
            if (!lock)
                return false;

            bool fastWriteReadback{buffer->CanUseFastWriteReadback()};
            if (fastWriteReadback ? buffer->writeCycle : buffer->cycle)
                return false;

            if (fastWriteReadback) {
                // The backing is safe to read once the last GPU writer has completed. Later
                // read-only GPU users may continue using the backing while the guest write updates
                // only the mirror. Any future mirror-to-backing sync still waits on the full cycle.
                // The imminent CPU write must always leave the guest copy CPU dirty.
                std::memcpy(buffer->mirror.data(), buffer->backing->data(), buffer->mirror.size());
                buffer->dirtyState = DirtyState::CpuDirty;
                buffer->fastWriteReadbackHits++;
                bufferFastWriteHits.fetch_add(1, std::memory_order_relaxed);
                buffer->LogFastWriteReadbackDiag();
                LogBufferReadbackDiag();
                return true;
            }

            bufferWritePreciseFallbacks.fetch_add(1, std::memory_order_relaxed);
            LogBufferReadbackDiag();
            buffer->SynchronizeGuest(true); // We need to assume the buffer is dirty since we don't know what the guest is writing
            buffer->dirtyState = DirtyState::CpuDirty;

            return true;
        });
    }

    void Buffer::EnableTrackedShadowDirect() {
        if (!directTrackedShadowActive) {
            directTrackedShadow.resize(guest->size());
            directTrackedShadowActive = true;
        }
    }

    span<u8> Buffer::BeginWriteCpuSequencedDirect(size_t offset, size_t size) {
        EnableTrackedShadowDirect();
        directTrackedWrites.Insert({offset, offset + size});
        return {directTrackedShadow.data() + offset, size};
    }

    bool Buffer::RefreshGpuReadsActiveDirect() {
        bool readsActive{SequencedCpuBackingWritesBlocked() || !PollFence()};
        if (!readsActive) {
            if (directTrackedShadowActive) {
                directTrackedShadowActive = false;
                directTrackedShadow.clear();
                directTrackedShadow.shrink_to_fit();
            }
            directTrackedWrites.Clear();
        }
        
        return readsActive;
    }
    
    bool Buffer::RefreshGpuWritesActiveDirect(bool wait, const std::function<void()> &flushHostCallback) {
        if (directGpuWritesActive && (!PollFence() || AllCpuBackingWritesBlocked())) {
            if (wait) {
                if (AllCpuBackingWritesBlocked()) // If we are dirty in the current cycle we'll need to flush
                    flushHostCallback();

                WaitOnFence();

                // No longer dirty
            } else {
                return true;
            }
        }

        directGpuWritesActive = false;
        return false;
    }

    bool Buffer::ValidateMegaBufferViewImplDirect(vk::DeviceSize size) {
        if (!everHadInlineUpdate || size >= MegaBufferChunkSize)
            // Don't megabuffer buffers that have never had inline updates
            return false;

        if (RefreshGpuWritesActiveDirect())
            // If the buffer is currently being written to by the GPU then we can't megabuffer it
            return false;

        if (directTrackedShadowActive)
            // If the mirror contents aren't fully up to date then we can't megabuffer that would ignore any shadow tracked writes
            return false;

        return true;
    }

    bool Buffer::ValidateMegaBufferViewImplStaged(vk::DeviceSize size) {
        if ((!everHadInlineUpdate && sequenceNumber < FrequentlySyncedThreshold) || size >= MegaBufferChunkSize)
            // Don't megabuffer buffers that have never had inline updates and are not frequently synced since performance is only going to be harmed as a result of the constant copying and there wont be any benefit since there are no GPU inline updates that would be avoided
            return false;

        // We are safe to check dirty state here since it will only ever be set GPU dirty with the buffer locked and from the active GPFIFO thread. This helps with perf since the lock ends up being slightly expensive
        if (dirtyState == DirtyState::GpuDirty)
            // Bail out if buffer cannot be synced, we don't know the contents ahead of time so the sequence is indeterminate
            return false;

        return true;
    }

    bool Buffer::ValidateMegaBufferView(vk::DeviceSize size) {
        return isDirect ? ValidateMegaBufferViewImplDirect(size) : ValidateMegaBufferViewImplStaged(size);
    }

    void Buffer::CopyFromImplDirect(vk::DeviceSize dstOffset,
                                    Buffer *src, vk::DeviceSize srcOffset, vk::DeviceSize size,
                                    UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        everHadInlineUpdate = true;
        bool needsGpuTracking{src->RefreshGpuWritesActiveDirect() || RefreshGpuWritesActiveDirect()};
        bool needsCpuTracking{RefreshGpuReadsActiveDirect() && !needsGpuTracking};
        if (needsGpuTracking || needsCpuTracking) {
            if (needsGpuTracking) // Force buffer to be dirty for this cycle if either of the sources are dirty, this is needed as otherwise it could have just been dirty from the previous cycle
                MarkGpuDirty(usageTracker);
            gpuCopyCallback();

            if (needsCpuTracking)
                src->Read(false, {}, BeginWriteCpuSequencedDirect(dstOffset, size), srcOffset);
        } else {
            src->Read(false, {}, {mirror.data() + dstOffset, size}, srcOffset);
        }
    }

    void Buffer::CopyFromImplStaged(vk::DeviceSize dstOffset,
                                    Buffer *src, vk::DeviceSize srcOffset, vk::DeviceSize size,
                                    UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        std::scoped_lock lock{stateMutex, src->stateMutex}; // Fine even if src and dst are same since recursive mutex

        if (dirtyState == DirtyState::CpuDirty && SequencedCpuBackingWritesBlocked())
            // If the buffer is used in sequence directly on the GPU, SynchronizeHost before modifying the mirror contents to ensure proper sequencing. This write will then be sequenced on the GPU instead (the buffer will be kept clean for the rest of the execution due to gpuCopyCallback blocking all writes)
            SynchronizeHost();

        if (dirtyState != DirtyState::GpuDirty && src->dirtyState != DirtyState::GpuDirty) {
            std::memcpy(mirror.data() + dstOffset, src->mirror.data() + srcOffset, size);

            if (dirtyState == DirtyState::CpuDirty && !SequencedCpuBackingWritesBlocked())
                // Skip updating backing if the changes are gonna be updated later by SynchroniseHost in executor anyway
                return;

            if (!SequencedCpuBackingWritesBlocked() && PollFence()) {
                // We can write directly to the backing as long as this resource isn't being actively used by a past workload (in the current context or another)
                std::memcpy(backing->data() + dstOffset, src->mirror.data() + srcOffset, size);
            } else {
                // The GPU copy callback reads from the source backing. If the source is CPU dirty,
                // its mirror is newer than that backing, so blocking backing writes first would leave
                // the GPU reading stale data and create the invalid CpuDirty + AllWrites state.
                // Synchronize the source before the callback makes it immutable for this execution.
                if (src->dirtyState == DirtyState::CpuDirty)
                    src->SynchronizeHost();

                gpuCopyCallback();
            }
        } else {
            // If only the destination is GPU dirty, the source can still be CPU dirty. The GPU
            // callback will read the source backing, so make it current before either buffer is
            // made immutable by the interconnect copy path.
            if (src->dirtyState == DirtyState::CpuDirty)
                src->SynchronizeHost();

            MarkGpuDirty(usageTracker);
            gpuCopyCallback();
        }
    }

    bool Buffer::WriteImplDirect(span<u8> data, vk::DeviceSize offset,
                                 UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        // If the buffer is GPU dirty do the write on the GPU and we're done
        if (RefreshGpuWritesActiveDirect()) {
            if (gpuCopyCallback) {
                // Propagate dirtiness to the current cycle, since if this is only dirty in a previous cycle that could change at any time and we would need to have the write saved somewhere for CPU reads
                // By propagating the dirtiness to the current cycle we can avoid this and force a wait on any reads
                MarkGpuDirty(usageTracker);
                gpuCopyCallback();
                return false;
            } else {
                return true;
            }
        }

        if (RefreshGpuReadsActiveDirect()) {
            // If the GPU could read the buffer we need to track the write in the shadow and do the actual write on the GPU
            if (gpuCopyCallback)
                gpuCopyCallback();
            else
                return true;

            BeginWriteCpuSequencedDirect(offset, data.size()).copy_from(data);
            return false;
        }

        // If the GPU isn't accessing the mirror we can just write directly to it
        std::memcpy(mirror.data() + offset, data.data(), data.size());
        return false;
    }

    bool Buffer::WriteImplStaged(span<u8> data, vk::DeviceSize offset, const std::function<void()> &gpuCopyCallback) {
        // We cannot have *ANY* state changes for the duration of this function, if the buffer became CPU dirty partway through the GPU writes would mismatch the CPU writes
        std::scoped_lock lock{stateMutex};

        // If the buffer is GPU dirty do the write on the GPU and we're done
        if (dirtyState == DirtyState::GpuDirty) {
            if (gpuCopyCallback)
                gpuCopyCallback();
            else
                return true;
        }

        if (dirtyState == DirtyState::CpuDirty && SequencedCpuBackingWritesBlocked())
            // If the buffer is used in sequence directly on the GPU, SynchronizeHost before modifying the mirror contents to ensure proper sequencing. This write will then be sequenced on the GPU instead (the buffer will be kept clean for the rest of the execution due to gpuCopyCallback blocking all writes)
            SynchronizeHost();

        std::memcpy(mirror.data() + offset, data.data(), data.size()); // Always copy to mirror since any CPU side reads will need the up-to-date contents

        if (dirtyState == DirtyState::CpuDirty && !SequencedCpuBackingWritesBlocked())
            // Skip updating backing if the changes are gonna be updated later by SynchroniseHost in executor anyway
            return false;

        if (!SequencedCpuBackingWritesBlocked() && PollFence()) {
            // We can write directly to the backing as long as this resource isn't being actively used by a past workload (in the current context or another)
            std::memcpy(backing->data() + offset, data.data(), data.size());
        } else {
            // If this buffer is host immutable, perform a GPU-side inline update for the buffer contents since we can't directly modify the backing
            // If no copy callback is supplied, return true to indicate that the caller should repeat the write with an appropriate callback
            if (gpuCopyCallback)
                gpuCopyCallback();
            else
                return true;
        }

        return false;
    }

    void Buffer::ReadImplDirect(const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset) {
        // If GPU writes are active then wait until that's no longer the case
        RefreshGpuWritesActiveDirect(true, flushHostCallback);

        if (directTrackedShadowActive && RefreshGpuReadsActiveDirect()) {
            size_t dstOffset{};
            while (dstOffset != data.size()) {
                auto srcOffset{dstOffset + offset};
                auto dstRemaining{data.size() - dstOffset};
                auto result{directTrackedWrites.Query(srcOffset)};
                auto size{result.size ? std::min(result.size, dstRemaining) : dstRemaining};
                auto srcData{result.enclosed ? directTrackedShadow.data() : mirror.data()};
                std::memcpy(data.data() + dstOffset, srcData + srcOffset, size);
                dstOffset += size;
            }
        } else [[likely]] {
            std::memcpy(data.data(), mirror.data() + offset, data.size());
        }
    }

    void Buffer::ReadImplStaged(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset) {
        if (dirtyState == DirtyState::GpuDirty)
            SynchronizeGuestImmediate(isFirstUsage, flushHostCallback);

        std::memcpy(data.data(), mirror.data() + offset, data.size());
    }

    void Buffer::MarkGpuDirtyImplDirect() {
        directGpuWritesActive = true;
        BlockAllCpuBackingWrites();
        AdvanceSequence();
    }

    void Buffer::MarkGpuDirtyImplStaged() {
        std::scoped_lock lock{stateMutex}; // stateMutex is locked to prevent state changes at any point during this function

        if (dirtyState == DirtyState::GpuDirty)
            return;

        gpu.state.process->trap.TrapRegions(*trapHandle, false); // This has to occur prior to any synchronization as it'll skip trapping

        if (dirtyState == DirtyState::CpuDirty)
            SynchronizeHost(true); // Will transition the Buffer to Clean

        dirtyState = DirtyState::GpuDirty;

        BlockAllCpuBackingWrites();
        AdvanceSequence(); // The GPU will modify buffer contents so advance to the next sequence
    }

    void Buffer::MarkGpuDirtyImpl() {
        currentExecutionGpuDirty = true;

        if (isDirect)
            MarkGpuDirtyImplDirect();
        else
            MarkGpuDirtyImplStaged();
    }

    bool Buffer::CanUseFastWriteReadback() const {
        return !isDirect &&
               accumulatedGuestWaitTime > FastReadbackHackWaitTimeThreshold &&
               *gpu.state.settings->enableFastGpuReadbackHack &&
               *gpu.state.settings->enableFastReadbackWrites;
    }

    void Buffer::LogFastWriteReadbackDiag() const {
        const bool milestone{
            fastWriteReadbackHits == 1 ||
            fastWriteReadbackHits == 8 ||
            fastWriteReadbackHits == 32 ||
            fastWriteReadbackHits == 128 ||
            fastWriteReadbackHits == 512 ||
            (fastWriteReadbackHits > 512 && (fastWriteReadbackHits % 1024) == 0)
        };
        if (!milestone)
            return;

        LOGI("[FastReadbackDiag][HotBuffer] id={} size={} sequence={} fast_hits={} read_only_bypasses={} guest_waits={} guest_wait_us={} cycle_present={} write_cycle_present={} cycle_is_write_cycle={} src_internal={} src_storage={} src_image={} src_query={} src_xfb={} src_dma_clear={} storage_vtx={} storage_tesc={} storage_tese={} storage_geom={} storage_frag={} storage_comp={} storage_other={} storage_min={} storage_max={} writer_page_overlap={} writer_page_disjoint={} writer_page_unknown={}",
             id,
             mirror.size(),
             sequenceNumber,
             fastWriteReadbackHits,
             fastWriteReadbackBypasses,
             accumulatedGuestWaitCounter,
             accumulatedGuestWaitTime.count() / 1000,
             static_cast<bool>(cycle),
             static_cast<bool>(writeCycle),
             cycle && writeCycle && cycle == writeCycle,
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::Internal)],
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::StorageBuffer)],
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::ImageBuffer)],
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::Query)],
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::TransformFeedback)],
             gpuWriteSourceCounts[static_cast<size_t>(GpuWriteSource::DmaClear)],
             storageWriteVertex,
             storageWriteTessControl,
             storageWriteTessEvaluation,
             storageWriteGeometry,
             storageWriteFragment,
             storageWriteCompute,
             storageWriteOther,
             storageWriteMinBindingSize,
             storageWriteMaxBindingSize,
             writeFaultWriterRangeOverlap,
             writeFaultWriterRangeDisjoint,
             writeFaultWriterRangeUnknown);
    }

    Buffer::Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, GuestBuffer guest, size_t id, bool direct)
        : gpu{gpu},
          guest{guest},
      mirror{gpu.state.process->memory.CreateMirror(guest)},
          delegate{delegateAllocator.EmplaceUntracked<BufferDelegate>(this)},
          isDirect{direct},
          id{id},
          megaBufferTableShift{std::max(std::bit_width(guest.size() / MegaBufferTableMaxEntries - 1), MegaBufferTableShiftMin)} {
        if (isDirect)
            directBacking = gpu.memory.ImportBuffer(mirror);
        else
            backing = gpu.memory.AllocateBuffer(mirror.size());

        megaBufferTable.resize(guest.size() / (1 << megaBufferTableShift));
    }

    Buffer::Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, vk::DeviceSize size, size_t id)
        : gpu{gpu},
          backing{gpu.memory.AllocateBuffer(size)},
          delegate{delegateAllocator.EmplaceUntracked<BufferDelegate>(this)},
          id{id} {
        dirtyState = DirtyState::Clean; // Since this is a host-only buffer it's always going to be clean
    }

    Buffer::~Buffer() {
        if (trapHandle)
            gpu.state.process->trap.DeleteTrap(*trapHandle);
        SynchronizeGuest(true);
        if (mirror.valid())
            munmap(mirror.data(), mirror.size());
        WaitOnFence();
    }

    void Buffer::RecordStorageWriteBinding(vk::PipelineStageFlagBits stage, size_t bindingOffset, size_t bindingSize) {
        if (!storageWriteMinBindingSize || bindingSize < storageWriteMinBindingSize)
            storageWriteMinBindingSize = bindingSize;
        if (bindingSize > storageWriteMaxBindingSize)
            storageWriteMaxBindingSize = bindingSize;

        if (bindingOffset < mirror.size()) {
            const size_t bindingEnd{std::min(mirror.size(), bindingOffset + bindingSize)};
            if (bindingEnd > bindingOffset) {
                currentExecutionStorageWriteRanges.Insert({bindingOffset, bindingEnd});
                currentExecutionStorageRangeRecorded = true;
            }
        }

        switch (stage) {
            case vk::PipelineStageFlagBits::eVertexShader:
                storageWriteVertex++;
                break;
            case vk::PipelineStageFlagBits::eTessellationControlShader:
                storageWriteTessControl++;
                break;
            case vk::PipelineStageFlagBits::eTessellationEvaluationShader:
                storageWriteTessEvaluation++;
                break;
            case vk::PipelineStageFlagBits::eGeometryShader:
                storageWriteGeometry++;
                break;
            case vk::PipelineStageFlagBits::eFragmentShader:
                storageWriteFragment++;
                break;
            case vk::PipelineStageFlagBits::eComputeShader:
                storageWriteCompute++;
                break;
            default:
                storageWriteOther++;
                break;
        }
    }

    void Buffer::MarkGpuDirty(UsageTracker &usageTracker, GpuWriteSource source) {
        if (!guest)
            return;

        gpuWriteSourceCounts[static_cast<size_t>(source)]++;
        if (source != GpuWriteSource::StorageBuffer)
            currentExecutionStorageRangesComplete = false;
        usageTracker.dirtyIntervals.Insert(*guest);
        MarkGpuDirtyImpl();
    }

    void Buffer::WaitOnFence() {
        TRACE_EVENT("gpu", "Buffer::WaitOnFence");

        if (cycle) {
            cycle->Wait();
            cycle = nullptr;
            writeCycle = nullptr;
            writeCycleStorageWriteRanges.Clear();
            writeCycleStorageRangesComplete = false;
        }
    }

    bool Buffer::PollFence() {
        if (!cycle)
            return true;

        if (cycle->Poll()) {
            cycle = nullptr;
            writeCycle = nullptr;
            writeCycleStorageWriteRanges.Clear();
            writeCycleStorageRangesComplete = false;
            return true;
        }

        return false;
    }

    void Buffer::Invalidate() {
        if (trapHandle) {
            gpu.state.process->trap.DeleteTrap(*trapHandle);
            trapHandle = {};
        }

        // Will prevent any sync operations so even if the trap handler is partway through running and hasn't yet acquired the lock it won't do anything
        guest = {};
    }

    void Buffer::SynchronizeHost(bool skipTrap) {
        if (!guest || isDirect)
            return;

        TRACE_EVENT("gpu", "Buffer::SynchronizeHost");

        {
            std::scoped_lock lock{stateMutex};
            if (dirtyState != DirtyState::CpuDirty)
                return;

            dirtyState = DirtyState::Clean;
            WaitOnFence();

            AdvanceSequence(); // We are modifying GPU backing contents so advance to the next sequence

            if (!skipTrap)
                gpu.state.process->trap.TrapRegions(*trapHandle, true); // Trap any future CPU writes to this buffer, must be done before the memcpy so that any modifications during the copy are tracked
        }

        std::memcpy(backing->data(), mirror.data(), mirror.size());
    }

    bool Buffer::SynchronizeGuest(bool skipTrap, bool nonBlocking) {
        if (!guest || isDirect)
            return false;

        TRACE_EVENT("gpu", "Buffer::SynchronizeGuest");

        {
            std::scoped_lock lock{stateMutex};

            if (dirtyState != DirtyState::GpuDirty)
                return true; // If the buffer is not dirty, there is no need to synchronize it

            if (nonBlocking && !PollFence())
                return false; // If the fence is not signalled and non-blocking behaviour is requested then bail out

            i64 syncStartNs{util::GetTimeNs()};
            WaitOnFence();
            std::memcpy(mirror.data(), backing->data(), mirror.size());
            i64 syncNs{util::GetTimeNs() - syncStartNs};

            dirtyState = DirtyState::Clean;
            bufferPreciseSyncs.fetch_add(1, std::memory_order_relaxed);
            bufferPreciseSyncNs.fetch_add(static_cast<u64>(syncNs), std::memory_order_relaxed);
            LogBufferReadbackDiag();
        }

        if (!skipTrap)
            gpu.state.process->trap.TrapRegions(*trapHandle, true);

        return true;
    }

    void Buffer::SynchronizeGuestImmediate(bool isFirstUsage, const std::function<void()> &flushHostCallback) {
        if (isDirect)
            return;

        // If this buffer was attached to the current cycle, flush all pending host GPU work and wait to ensure that we read valid data
        if (!isFirstUsage)
            flushHostCallback();

        SynchronizeGuest();
    }

    void Buffer::Read(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset) {
        if (isDirect)
            ReadImplDirect(flushHostCallback, data, offset);
        else
            ReadImplStaged(isFirstUsage, flushHostCallback, data, offset);
    }

    bool Buffer::Write(span<u8> data, vk::DeviceSize offset, UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        AdvanceSequence(); // We are modifying GPU backing contents so advance to the next sequence
        everHadInlineUpdate = true;

        usageTracker.sequencedIntervals.Insert(*guest);

        if (isDirect)
            return WriteImplDirect(data, offset, usageTracker, gpuCopyCallback);
        else
            return WriteImplStaged(data, offset, gpuCopyCallback);
    }

    void Buffer::CopyFrom(vk::DeviceSize dstOffset,
                          Buffer *src, vk::DeviceSize srcOffset, vk::DeviceSize size,
                          UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        AdvanceSequence(); // We are modifying GPU backing contents so advance to the next sequence
        everHadInlineUpdate = true;

        usageTracker.sequencedIntervals.Insert(*guest);

        if (isDirect)
            CopyFromImplDirect(dstOffset, src, srcOffset, size, usageTracker, gpuCopyCallback);
        else
            CopyFromImplStaged(dstOffset, src, srcOffset, size, usageTracker, gpuCopyCallback);
    }

    BufferView Buffer::GetView(vk::DeviceSize offset, vk::DeviceSize size) {
        return BufferView{delegate, offset, size};
    }

    BufferView Buffer::TryGetView(span<u8> mapping) {
        if (guest->contains(mapping))
            return GetView(static_cast<vk::DeviceSize>(std::distance(guest->begin(), mapping.begin())), mapping.size());
        else
            return {};
    }

    BufferBinding Buffer::TryMegaBufferView(const std::shared_ptr<FenceCycle> &pCycle, MegaBufferAllocator &allocator, ContextTag executionTag,
                                            vk::DeviceSize offset, vk::DeviceSize size) {
        if (!ValidateMegaBufferView(size))
            return {};

        // If the active execution has changed all previous allocations are now invalid
        if (executionTag != lastExecutionTag) [[unlikely]] {
            ResetMegabufferState();
            lastExecutionTag = executionTag;
        }

        // If more than half the buffer has been megabuffered in chunks within the same execution assume this will generally be the case for this buffer and just megabuffer the whole thing without chunking
        if (unifiedMegaBufferEnabled || (megaBufferViewAccumulatedSize > (mirror.size() / 2) && mirror.size() < MegaBufferChunkSize)) {
            if (!unifiedMegaBuffer) {
                unifiedMegaBuffer = allocator.Push(pCycle, mirror, true);
                unifiedMegaBufferEnabled = true;
            }

            return BufferBinding{unifiedMegaBuffer.buffer, unifiedMegaBuffer.offset + offset, size};
        }

        if (size > MegaBufferingDisableThreshold) {
            megaBufferViewAccumulatedSize += size;
            return {};
        }

        size_t entryIdx{offset >> megaBufferTableShift};
        size_t bufferEntryOffset{entryIdx << megaBufferTableShift};
        size_t entryViewOffset{offset - bufferEntryOffset};

        if (entryIdx >= megaBufferTable.size())
            return {};

        auto &allocation{megaBufferTable[entryIdx]};

        // If the cached allocation is invalid or too small, allocate a new one
        if (!megaBufferTableValidity.test(entryIdx) || allocation.region.size() < (size + entryViewOffset)) {
            // Use max(oldSize, newSize) to avoid redundant reallocations within an execution if a larger allocation comes along later
            auto mirrorAllocationRegion{mirror.subspan(bufferEntryOffset, std::max(entryViewOffset + size, allocation.region.size()))};
            allocation = allocator.Push(pCycle, mirrorAllocationRegion, true);
            megaBufferTableValidity.set(entryIdx);
            megaBufferViewAccumulatedSize += mirrorAllocationRegion.size();
            megaBufferTableUsed = true;
        }

        return {allocation.buffer, allocation.offset + entryViewOffset, size};
    }

    void Buffer::AdvanceSequence() {
        ResetMegabufferState();
        sequenceNumber++;
    }

    span<u8> Buffer::GetReadOnlyBackingSpan(bool isFirstUsage, const std::function<void()> &flushHostCallback) {
        if (!isDirect) {
            std::unique_lock lock{stateMutex};
            if (dirtyState == DirtyState::GpuDirty)
                SynchronizeGuestImmediate(isFirstUsage, flushHostCallback);
        } else {
            RefreshGpuWritesActiveDirect(true, flushHostCallback);
        }

        return mirror;
    }

    void Buffer::PopulateReadBarrier(vk::PipelineStageFlagBits dstStage, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (currentExecutionGpuDirty) {
            srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
            dstStageMask |= dstStage;
        }
    }

    void Buffer::lock() {
        mutex.lock();
        accumulatedCpuLockCounter++;
    }

    bool Buffer::LockWithTag(ContextTag pTag) {
        if (pTag && pTag == tag)
            return false;

        mutex.lock();
        tag = pTag;
        return true;
    }

    void Buffer::unlock() {
        tag = ContextTag{};
        AllowAllBackingWrites();
        currentExecutionGpuDirty = false;
        mutex.unlock();
    }

    bool Buffer::try_lock() {
        if (mutex.try_lock()) {
            accumulatedCpuLockCounter++;
            return true;
        }
        return false;
    }

    BufferDelegate::BufferDelegate(Buffer *buffer) : buffer{buffer} {}

    Buffer *BufferDelegate::GetBuffer() {
        if (linked) [[unlikely]]
            return link->GetBuffer();
        else
            return buffer;
    }

    void BufferDelegate::Link(BufferDelegate *newTarget, vk::DeviceSize newOffset) {
        if (linked)
            throw exception("Cannot link a buffer delegate that is already linked!");

        linked = true;
        link = newTarget;
        offset = newOffset;
    }

    vk::DeviceSize BufferDelegate::GetOffset() {
        if (linked) [[unlikely]]
            return link->GetOffset() + offset;
        else
            return offset;
    }

    void BufferView::ResolveDelegate() {
        offset += delegate->GetOffset();
        delegate = delegate->GetBuffer()->delegate;
    }

    BufferView::BufferView() {}

    BufferView::BufferView(BufferDelegate *delegate, vk::DeviceSize offset, vk::DeviceSize size) : delegate{delegate}, offset{offset}, size{size} {}

    Buffer *BufferView::GetBuffer() const {
        return delegate->GetBuffer();
    }

    BufferBinding BufferView::GetBinding(GPU &gpu) const {
        std::scoped_lock lock{gpu.buffer.recreationMutex};
        return {delegate->GetBuffer()->GetBacking(), offset + delegate->GetOffset(), size};
    }

    vk::DeviceSize BufferView::GetOffset() const {
        return offset + delegate->GetOffset();
    }

    void BufferView::Read(bool isFirstUsage, const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize readOffset) const {
        GetBuffer()->Read(isFirstUsage, flushHostCallback, data, readOffset + GetOffset());
    }

    bool BufferView::Write(span<u8> data, vk::DeviceSize writeOffset, UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) const {
        return GetBuffer()->Write(data, writeOffset + GetOffset(), usageTracker, gpuCopyCallback);
    }

    BufferBinding BufferView::TryMegaBuffer(const std::shared_ptr<FenceCycle> &pCycle, MegaBufferAllocator &allocator, ContextTag executionTag, size_t sizeOverride) const {
        return GetBuffer()->TryMegaBufferView(pCycle, allocator, executionTag, GetOffset(), sizeOverride ? sizeOverride : size);
    }

    span<u8> BufferView::GetReadOnlyBackingSpan(bool isFirstUsage, const std::function<void()> &flushHostCallback) {
        auto backing{delegate->GetBuffer()->GetReadOnlyBackingSpan(isFirstUsage, flushHostCallback)};
        return backing.subspan(GetOffset(), size);
    }

    void BufferView::CopyFrom(BufferView src, UsageTracker &usageTracker, const std::function<void()> &gpuCopyCallback) {
        if (src.size != size)
            throw exception("Copy size mismatch!");
        return GetBuffer()->CopyFrom(GetOffset(), src.GetBuffer(), src.GetOffset(), size, usageTracker, gpuCopyCallback);
    }
}
