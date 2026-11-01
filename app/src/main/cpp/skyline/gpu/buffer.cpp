// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <adrenotools/driver.h>
#include <gpu.h>
#include <kernel/memory.h>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include <common/settings.h>
#include "buffer.h"

namespace skyline::gpu {
    void Buffer::ResetMegabufferState() {
        if (megaBufferTableUsed)
            megaBufferTableValidity.reset();

        megaBufferTableUsed = false;
        megaBufferViewAccumulatedSize = 0;
        unifiedMegaBuffer = {};
    }

    void Buffer::SetupStagedTraps() {
        if (!isDirect && guest)
            RebuildStagedTraps();
    }

    void Buffer::RebuildStagedTraps() {
        if (!guest || isDirect)
            return;
        std::weak_ptr<Buffer> weakThis{shared_from_this()};
        std::vector<StagedTrap> next;
        next.reserve(stagedRegions.Regions().size());
        try {
            for (const auto& region : stagedRegions.Regions()) {
                const auto begin{region.begin}, end{region.end};
                span<u8> mapping{guest->data() + begin, end - begin};
                auto handle{gpu.state.process->trap.CreateTrap(
                    mapping,
                    [weakThis, begin, end] {
                        auto buffer{weakThis.lock()};
                        if (!buffer)
                            return;
                        std::shared_ptr<FenceCycle> waited{};
                        bool completed{}, measure{};
                        std::chrono::nanoseconds elapsed{};
                        for (;;) {
                            {
                                std::scoped_lock resourceLock{*buffer};
                                std::scoped_lock stateLock{buffer->stateMutex};
                                if (completed) {
                                    ++buffer->accumulatedGuestWaitCounter;
                                    buffer->accumulatedGuestWaitTime += elapsed;
                                    completed = false;
                                }
                                if (!buffer->guest)
                                    return;
                                if (!buffer->AllCpuBackingWritesBlocked() && !buffer->stagedRegions.Has(BufferRegionState::Owner::Gpu, begin, end - begin))
                                    return;
                                if (waited && buffer->cycle == waited) {
                                    buffer->cycle = {};
                                    return;
                                }
                                waited = buffer->cycle;
                                measure = buffer->accumulatedGuestWaitCounter > FastReadbackHackWaitCountThreshold;
                            }
                            if (!waited)
                                return;
                            const auto start{measure ? util::GetTimeNs() : 0};
                            waited->Wait();
                            elapsed = measure ? std::chrono::nanoseconds(util::GetTimeNs() - start) : std::chrono::nanoseconds{};
                            completed = true;
                        }
                    },
                    [weakThis, begin, end] {
                        auto buffer{weakThis.lock()};
                        if (!buffer)
                            return true;
                        std::unique_lock stateLock{buffer->stateMutex, std::try_to_lock};
                        if (!stateLock)
                            return false;
                        if (!buffer->guest || !buffer->stagedRegions.Has(BufferRegionState::Owner::Gpu, begin, end - begin))
                            return true;
                        std::unique_lock resourceLock{*buffer, std::try_to_lock};
                        if (!resourceLock || buffer->cycle)
                            return false;
                        return buffer->SynchronizeGuestRegion(true, false, begin, end - begin);
                    },
                    [weakThis, begin, end] {
                        auto buffer{weakThis.lock()};
                        if (!buffer)
                            return true;
                        std::unique_lock stateLock{buffer->stateMutex, std::try_to_lock};
                        if (!stateLock)
                            return false;
                        if (!buffer->guest)
                            return true;
                        const bool gpuDirty{buffer->stagedRegions.Has(BufferRegionState::Owner::Gpu, begin, end - begin)};
                        if (!buffer->AllCpuBackingWritesBlocked() && !gpuDirty) {
                            buffer->MarkStagedCpuWrite(begin, end - begin, true);
                            return true;
                        }
                        if (gpuDirty && buffer->accumulatedGuestWaitTime > FastReadbackHackWaitTimeThreshold &&
                            *buffer->gpu.state.settings->enableFastGpuReadbackHack) {
                            // Preserve unrelated CPU ownership even in the existing unsafe shortcut.
                            for (const auto r : buffer->stagedRegions.Regions())
                                if (r.owner == BufferRegionState::Owner::Gpu && r.begin < end && begin < r.end) {
                                    const auto lo{std::max(begin, r.begin)}, hi{std::min(end, r.end)};
                                    std::memcpy(buffer->mirror.data() + lo, buffer->backing->data() + lo, hi - lo);
                                    buffer->stagedRegions.AssignExisting(lo, hi - lo,
                                                                         *buffer->gpu.state.settings->enableFastReadbackWrites
                                                                             ? BufferRegionState::Owner::Cpu
                                                                             : BufferRegionState::Owner::Synced);
                                }
                            buffer->RefreshStagedDirtyState();
                            return true;
                        }
                        std::unique_lock resourceLock{*buffer, std::try_to_lock};
                        if (!resourceLock || buffer->cycle)
                            return false;
                        if (!buffer->SynchronizeGuestRegion(true, false, begin, end - begin))
                            return false;
                        buffer->MarkStagedCpuWrite(begin, end - begin, true);
                        return true;
                    })};
                next.push_back({begin, end, handle});
                if (region.owner != BufferRegionState::Owner::Cpu)
                    gpu.state.process->trap.TrapRegions(handle, region.owner != BufferRegionState::Owner::Gpu);
            }
        } catch (...) {
            for (const auto& entry : next)
                gpu.state.process->trap.DeleteTrap(entry.handle);
            throw;
        }
        // Replacement protections are live before any old protection is removed.
        auto previous{std::move(stagedTraps)};
        stagedTraps = std::move(next);
        for (const auto& entry : previous)
            gpu.state.process->trap.DeleteTrap(entry.handle);
    }

    void Buffer::PrepareStagedRange(vk::DeviceSize offset, vk::DeviceSize size) {
        if (stagedRegions.EnsureBounds(offset, size) || stagedTraps.size() != stagedRegions.Regions().size())
            RebuildStagedTraps();
    }

    void Buffer::ProtectStagedRange(size_t begin, size_t end, bool gpuDirty) {
        if (begin == end)
            return;
        for (const auto& entry : stagedTraps)
            if (entry.begin < end && begin < entry.end)
                gpu.state.process->trap.TrapRegions(entry.handle, !gpuDirty);
    }

    void Buffer::RefreshStagedDirtyState() {
        dirtyState = stagedRegions.Has(BufferRegionState::Owner::Gpu)   ? DirtyState::GpuDirty
                     : stagedRegions.Has(BufferRegionState::Owner::Cpu) ? DirtyState::CpuDirty
                                                                        : DirtyState::Clean;
    }

    bool Buffer::HasStagedCpuWrites() {
        std::scoped_lock stateLock{stateMutex};
        return stagedRegions.Has(BufferRegionState::Owner::Cpu);
    }

    void Buffer::MarkStagedCpuWrite(vk::DeviceSize offset, vk::DeviceSize size, bool skipTrap) {
        if (!size)
            return;
        if (!skipTrap)
            PrepareStagedRange(offset, size);
        stagedRegions.AssignExisting(offset, size, BufferRegionState::Owner::Cpu);
        RefreshStagedDirtyState();
        if (!skipTrap) {
            auto range{stagedRegions.Aligned(offset, size)};
            for (const auto& entry : stagedTraps)
                if (entry.begin < range.end && range.begin < entry.end)
                    gpu.state.process->trap.RemoveTrap(entry.handle);
        }
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
        if (!size)
            return;
        std::scoped_lock lock{stateMutex, src->stateMutex}; // Fine even if src and dst are same since recursive mutex
        PrepareStagedRange(dstOffset, size);

        if (dirtyState == DirtyState::CpuDirty && SequencedCpuBackingWritesBlocked())
            // If the buffer is used in sequence directly on the GPU, SynchronizeHost before modifying the mirror contents to ensure proper sequencing. This write will then be sequenced on the GPU instead (the buffer will be kept clean for the rest of the execution due to gpuCopyCallback blocking all writes)
            SynchronizeHost();

        if (dirtyState != DirtyState::GpuDirty && src->dirtyState != DirtyState::GpuDirty) {
            std::memcpy(mirror.data() + dstOffset, src->mirror.data() + srcOffset, size);

            if (dirtyState == DirtyState::CpuDirty && !SequencedCpuBackingWritesBlocked()) {
                MarkStagedCpuWrite(dstOffset, size);
                return;
            }

            if (!SequencedCpuBackingWritesBlocked() && PollFence()) {
                // We can write directly to the backing as long as this resource isn't being actively used by a past workload (in the current context or another)
                std::memcpy(backing->data() + dstOffset, src->mirror.data() + srcOffset, size);
            } else {
                // The GPU copy callback reads from the source backing. If the source is CPU dirty,
                // its mirror is newer than that backing, so blocking backing writes first would leave
                // the GPU reading stale data and create the invalid CpuDirty + AllWrites state.
                // Synchronize the source before the callback makes it immutable for this execution.
                if (src->HasStagedCpuWrites())
                    src->SynchronizeHostRegion(false, srcOffset, size);

                gpuCopyCallback();
            }
        } else {
            // If only the destination is GPU dirty, the source can still be CPU dirty. The GPU
            // callback will read the source backing, so make it current before either buffer is
            // made immutable by the interconnect copy path.
            if (src->HasStagedCpuWrites())
                src->SynchronizeHostRegion(false, srcOffset, size);

            MarkGpuDirty(usageTracker, dstOffset, size);
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

    bool Buffer::WriteImplStaged(span<u8> data, vk::DeviceSize offset, const std::function<void()>& gpuCopyCallback) {
        if (data.empty())
            return false;
        std::scoped_lock stateLock{stateMutex};
        PrepareStagedRange(offset, data.size());
        if (dirtyState == DirtyState::GpuDirty) {
            if (!gpuCopyCallback)
                return true;
            MarkGpuDirtyImpl(offset, data.size());
            gpuCopyCallback();
            return false;
        }
        if (dirtyState == DirtyState::CpuDirty && SequencedCpuBackingWritesBlocked())
            SynchronizeHost();
        std::memcpy(mirror.data() + offset, data.data(), data.size());
        if (dirtyState == DirtyState::CpuDirty && !SequencedCpuBackingWritesBlocked()) {
            MarkStagedCpuWrite(offset, data.size());
            return false;
        }
        if (!SequencedCpuBackingWritesBlocked() && PollFence())
            std::memcpy(backing->data() + offset, data.data(), data.size());
        else {
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
        if (data.empty())
            return;
        std::scoped_lock stateLock{stateMutex};
        if (stagedRegions.Has(BufferRegionState::Owner::Gpu, offset, data.size())) {
            if (!isFirstUsage)
                flushHostCallback();
            SynchronizeGuestRegion(false, false, offset, data.size());
        }
        std::memcpy(data.data(), mirror.data() + offset, data.size());
    }

    void Buffer::MarkGpuDirtyImplDirect() {
        directGpuWritesActive = true;
        BlockAllCpuBackingWrites();
        AdvanceSequence();
    }

    void Buffer::MarkGpuDirtyImplStaged(vk::DeviceSize offset, vk::DeviceSize size) {
        std::scoped_lock stateLock{stateMutex};
        if (size == VK_WHOLE_SIZE)
            size = mirror.size() - offset;
        PrepareStagedRange(offset, size);
        SynchronizeHostRegion(false, offset, size);
        auto range{stagedRegions.Aligned(offset, size)};
        ProtectStagedRange(range.begin, range.end, true);
        stagedRegions.AssignExisting(offset, size, BufferRegionState::Owner::Gpu);
        RefreshStagedDirtyState();
        BlockAllCpuBackingWrites(offset, size);
        AdvanceSequence();
    }

    void Buffer::MarkGpuDirtyImpl(vk::DeviceSize offset, vk::DeviceSize size) {
        currentExecutionGpuDirty = true;
        if (isDirect)
            MarkGpuDirtyImplDirect();
        else
            MarkGpuDirtyImplStaged(offset, size);
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
        else {
            backing = gpu.memory.AllocateBuffer(mirror.size());
            stagedRegions.Reset(mirror.size(), constant::PageSize);
        }

        megaBufferTable.resize(guest.size() / (1 << megaBufferTableShift));
    }

    Buffer::Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, vk::DeviceSize size, size_t id)
        : gpu{gpu},
          backing{gpu.memory.AllocateBuffer(size)},
          delegate{delegateAllocator.EmplaceUntracked<BufferDelegate>(this)},
          id{id} {
        dirtyState = DirtyState::Clean; // Since this is a host-only buffer it's always going to be clean
    }

    vk::raii::BufferView *Buffer::GetTexelBufferView(vk::DeviceSize offset, vk::DeviceSize size, vk::Format format) {
        const TexelBufferViewKey key{static_cast<VkBuffer>(GetBacking()), offset, size, static_cast<VkFormat>(format)};
        auto &view{texelBufferViewStore[key]};
        if (!view)
            view = std::make_unique<vk::raii::BufferView>(gpu.vkDevice, vk::BufferViewCreateInfo{
                .buffer = GetBacking(), .format = format, .offset = offset, .range = size,
            });
        return view.get();
    }

    Buffer::~Buffer() {
        for (const auto& entry : stagedTraps)
            gpu.state.process->trap.DeleteTrap(entry.handle);
        stagedTraps.clear();
        SynchronizeGuest(true);
        if (mirror.valid())
            munmap(mirror.data(), mirror.size());
        WaitOnFence();
    }

    void Buffer::MarkGpuDirty(UsageTracker& usageTracker, vk::DeviceSize offset, vk::DeviceSize size) {
        ++writeGeneration;
        if (!guest)
            return;
        usageTracker.dirtyIntervals.Insert(*guest);
        MarkGpuDirtyImpl(offset, size);
    }

    void Buffer::WaitOnFence() {
        TRACE_EVENT("gpu", "Buffer::WaitOnFence");

        if (cycle) {
            cycle->Wait();
            cycle = nullptr;
        }
    }

    bool Buffer::PollFence() {
        if (!cycle)
            return true;

        if (cycle->Poll()) {
            cycle = nullptr;
            return true;
        }

        return false;
    }

    void Buffer::Invalidate() {
        for (const auto& entry : stagedTraps)
            gpu.state.process->trap.DeleteTrap(entry.handle);
        stagedTraps.clear();
        guest = {};
    }

    void Buffer::SynchronizeHost(bool skipTrap) {
        SynchronizeHostRegion(skipTrap, 0, mirror.size());
    }

    void Buffer::SynchronizeHostForGpu() {
        if (!guest || isDirect)
            return;
        std::scoped_lock stateLock{stateMutex};
        if (stagedRegions.Accesses().empty())
            SynchronizeHostRegion(false, 0, mirror.size());
        else
            for (const auto range : stagedRegions.Accesses())
                SynchronizeHostRegion(false, range.begin, range.end - range.begin);
        stagedRegions.ClearAccesses();
    }

    void Buffer::SynchronizeHostRegion(bool skipTrap, vk::DeviceSize offset, vk::DeviceSize size) {
        if (!guest || isDirect)
            return;
        std::scoped_lock stateLock{stateMutex};
        if (!stagedRegions.Has(BufferRegionState::Owner::Cpu, offset, size))
            return;
        PrepareStagedRange(offset, size);
        const auto range{stagedRegions.Aligned(offset, size)};
        WaitOnFence(); // A backing mutation still needs all prior GPU users.
        AdvanceSequence();
        for (const auto r : stagedRegions.Regions())
            if (r.owner == BufferRegionState::Owner::Cpu && r.begin < range.end && range.begin < r.end) {
                if (!skipTrap)
                    ProtectStagedRange(r.begin, r.end, false);
                std::memcpy(backing->data() + r.begin, mirror.data() + r.begin, r.end - r.begin);
                stagedRegions.AssignExisting(r.begin, r.end - r.begin, BufferRegionState::Owner::Synced);
            }
        RefreshStagedDirtyState();
    }

    bool Buffer::SynchronizeGuest(bool skipTrap, bool nonBlocking) {
        return SynchronizeGuestRegion(skipTrap, nonBlocking, 0, mirror.size());
    }

    bool Buffer::SynchronizeGuestRegion(bool skipTrap, bool nonBlocking, vk::DeviceSize offset, vk::DeviceSize size) {
        if (!guest || isDirect)
            return false;
        std::scoped_lock stateLock{stateMutex};
        if (!stagedRegions.Has(BufferRegionState::Owner::Gpu, offset, size))
            return true;
        if (nonBlocking && !PollFence())
            return false;
        if (!skipTrap)
            PrepareStagedRange(offset, size);
        auto range{stagedRegions.Aligned(offset, size)};
        WaitOnFence();
        for (const auto r : stagedRegions.Regions())
            if (r.owner == BufferRegionState::Owner::Gpu && r.begin < range.end && range.begin < r.end) {
                const auto lo{std::max(r.begin, range.begin)}, hi{std::min(r.end, range.end)};
                std::memcpy(mirror.data() + lo, backing->data() + lo, hi - lo);
                stagedRegions.AssignExisting(lo, hi - lo, BufferRegionState::Owner::Synced);
                if (!skipTrap)
                    ProtectStagedRange(lo, hi, false);
            }
        RefreshStagedDirtyState();
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
        ++writeGeneration;
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
        ++writeGeneration;
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
