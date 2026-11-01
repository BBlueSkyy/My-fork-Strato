// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime.h"
#include <optional>
#include <ucontext.h>
#include <fcntl.h>
#include <thread>
#include <future>
#if HAS_BUFFER_REGIONS
#include <gpu/buffer_region_state.h>
#endif
namespace skyline {
    using ContextTag = uint64_t;
}
namespace skyline::gpu {
    struct FenceCycle {
        std::atomic<unsigned> waits{};
        std::atomic<bool> done{};
        std::mutex completionMutex;
        std::function<void()> complete;
        void Wait() {
            ++waits;
            std::scoped_lock lock{completionMutex};
            if (!done && complete)
                complete();
            done = true;
        }
        bool Poll() {
            return done;
        }
    };
    struct Backing {
        std::vector<u8> bytes;
        u8* external{};
        explicit Backing(size_t size) : bytes(size) {}
        u8* data() {
            return external ? external : bytes.data();
        }
    };
    struct Settings {
        std::shared_ptr<bool> enableFastGpuReadbackHack = std::make_shared<bool>(false), enableFastReadbackWrites = std::make_shared<bool>(false),
                              useDirectMemoryImport = std::make_shared<bool>(false);
    };
    struct Process {
        TrapManager trap;
        int fd{-1};
        u8* base{};
    };
    struct GPU {
        struct {
            Process* process;
            Settings* settings;
        } state;
    };
    struct FakeAllocator {};
    struct Buffer;
    struct Delegate {
        Buffer* buffer;
        Delegate* target{};
        size_t offset{};
        void Link(Delegate* p, size_t off) {
            target = p;
            offset = off;
        }
        Buffer* GetBuffer() {
            return target ? target->GetBuffer() : buffer;
        }
    };
    struct UsageTracker {
        struct {
            void Insert(auto) {}
        } dirtyIntervals;
    };
    struct Buffer : std::enable_shared_from_this<Buffer> {
        enum class DirtyState { Clean, CpuDirty, GpuDirty };
        DirtyState dirtyState{DirtyState::CpuDirty};
        GPU& gpu;
        std::optional<span<u8>> guest;
        span<u8> mirror;
        std::shared_ptr<Backing> backing;
        std::recursive_mutex mutex, stateMutex;
        std::shared_ptr<FenceCycle> cycle;
        enum class BackingImmutability { None, SequencedWrites, AllWrites } backingImmutability{};
        bool isDirect{}, currentExecutionGpuDirty{}, ownsMirror{}, everHadInlineUpdate{}, directTrackedShadowActive{};
        Delegate delegateValue{this};
        Delegate* delegate{&delegateValue};
        std::vector<u8> directTrackedShadow;
        struct {
            void Merge(auto&) {}
        } directTrackedWrites;
        bool RefreshGpuWritesActiveDirect(bool = false, const std::function<void()>& callback = {}) {
            return false;
        }
        void EnableTrackedShadowDirect() {}
        uint64_t tag{};
        unsigned sequence{}, writeGeneration{};
        static constexpr size_t FastReadbackHackWaitCountThreshold{6};
        static constexpr auto FastReadbackHackWaitTimeThreshold = std::chrono::nanoseconds(250000000);
        std::chrono::nanoseconds accumulatedGuestWaitTime{};
        size_t accumulatedGuestWaitCounter{};
#if HAS_BUFFER_REGIONS
        BufferRegionState stagedRegions;
        struct StagedTrap {
            size_t begin, end;
            TrapHandle handle;
        };
        std::vector<StagedTrap> stagedTraps;
        void RebuildStagedTraps();
        void PrepareStagedRange(vk::DeviceSize, vk::DeviceSize);
        void ProtectStagedRange(size_t, size_t, bool);
        void MarkStagedCpuWrite(vk::DeviceSize, vk::DeviceSize, bool = false);
        void RefreshStagedDirtyState();
        bool HasStagedCpuWrites();
        void SynchronizeHostRegion(bool, vk::DeviceSize, vk::DeviceSize);
        bool SynchronizeGuestRegion(bool, bool, vk::DeviceSize, vk::DeviceSize);
        void SynchronizeHostForGpu();
        void MarkGpuDirtyImplStaged(vk::DeviceSize = 0, vk::DeviceSize = ~uint64_t{});
#else
        std::optional<TrapHandle> trapHandle;
        void MarkGpuDirtyImplStaged();
#endif
        Buffer(GPU& gpu, span<u8> guest, span<u8> mirror) : gpu(gpu), guest(guest), mirror(mirror), backing(std::make_shared<Backing>(mirror.size())) {
#if HAS_BUFFER_REGIONS
            stagedRegions.Reset(mirror.size(), 4096);
#endif
        }
        Buffer(FakeAllocator&, GPU& gpu, span<u8> guest, size_t, bool direct)
            : Buffer(gpu, guest,
                     span<u8>{static_cast<u8*>(mmap(nullptr, guest.size(), PROT_READ | PROT_WRITE, MAP_SHARED, gpu.state.process->fd,
                                                    guest.data() - gpu.state.process->base)),
                              guest.size()}) {
            Check(mirror.data() != MAP_FAILED, "coalesce mirror");
            isDirect = direct;
            ownsMirror = true;
        }
        ~Buffer() {
            Invalidate();
            if (ownsMirror)
                munmap(mirror.data(), mirror.size());
        }
        void lock() {
            mutex.lock();
        }
        bool try_lock() {
            return mutex.try_lock();
        }
        void unlock() {
            tag = {};
            AllowAllBackingWrites();
            currentExecutionGpuDirty = false;
            mutex.unlock();
        }
        void AllowAllBackingWrites() {
            std::scoped_lock lock{stateMutex};
            backingImmutability = BackingImmutability::None;
        }
        void BlockSequencedCpuBackingWrites(vk::DeviceSize offset = 0, vk::DeviceSize size = ~uint64_t{}) {
#if HAS_BUFFER_REGIONS
            stagedRegions.Use(offset, size == ~uint64_t{} ? mirror.size() - offset : size);
#endif
            if (backingImmutability == BackingImmutability::None)
                backingImmutability = BackingImmutability::SequencedWrites;
        }
        void BlockAllCpuBackingWrites(vk::DeviceSize offset = 0, vk::DeviceSize size = ~uint64_t{}) {
#if HAS_BUFFER_REGIONS
            stagedRegions.Use(offset, size == ~uint64_t{} ? mirror.size() - offset : size);
#endif
            backingImmutability = BackingImmutability::AllWrites;
        }
        bool AllCpuBackingWritesBlocked() {
            return backingImmutability == BackingImmutability::AllWrites;
        }
        bool SequencedCpuBackingWritesBlocked() {
            return backingImmutability != BackingImmutability::None;
        }
        void AdvanceSequence() {
            ++sequence;
        }
        void MarkGpuDirtyImplDirect() {
            throw std::logic_error("unexpected DMI call");
        }
        void MarkGpuDirtyImpl(vk::DeviceSize = 0, vk::DeviceSize = ~uint64_t{});
        void MarkGpuDirty(UsageTracker&, vk::DeviceSize = 0, vk::DeviceSize = ~uint64_t{});
        void ReadImplStaged(bool, const std::function<void()>&, span<u8>, vk::DeviceSize);
        bool WriteImplStaged(span<u8>, vk::DeviceSize, const std::function<void()>&);
        void CopyFromImplStaged(vk::DeviceSize, Buffer*, vk::DeviceSize, vk::DeviceSize, UsageTracker&, const std::function<void()>&);
        void SynchronizeGuestImmediate(bool, const std::function<void()>&);
        void SetupStagedTraps();
        void WaitOnFence();
        bool PollFence();
        void Invalidate();
        void SynchronizeHost(bool = false);
        bool SynchronizeGuest(bool = false, bool = false);
    };
#if HAS_BUFFER_REGIONS
    struct BufferManager {
        GPU& gpu;
        FakeAllocator delegateAllocatorState;
        size_t nextBufferId{};
        std::mutex recreationMutex;
        struct LockedBuffer {
            std::shared_ptr<Buffer> buffer;
            struct Lock {
                bool first;
                bool IsFirstUsage() const {
                    return first;
                }
            } lock;
            std::unique_lock<Buffer> resourceLock;
            std::unique_lock<std::recursive_mutex> stateLock;
            LockedBuffer(std::shared_ptr<Buffer> p, ContextTag tag)
                : buffer(std::move(p)), lock{buffer->tag != tag}, resourceLock(*buffer), stateLock(buffer->stateMutex) {}
            Buffer* operator->() const {
                return buffer.get();
            }
        };
        using LockedBuffers = std::vector<LockedBuffer>;
        LockedBuffer CoalesceBuffers(span<u8>, const LockedBuffers&, ContextTag);
    };
#endif

} // namespace skyline::gpu
#include "buffer.inc"
#if HAS_BUFFER_REGIONS
#define LOGE(...) throw std::runtime_error("invalid coalesced CPU authority")
#include "coalesce.inc"
#undef LOGE
#endif
using skyline::gpu::Buffer;
static void Fault(int signal, siginfo_t* info, void*) {
    // Exact master NCE routing is extracted; it currently treats loads as writes.
    ArmContext ctx{};
    if (!RouteGuest(signal, info, &ctx))
        _exit(99);
}
struct Fixture {
    static constexpr size_t Size = 8192;
    int fd{memfd_create("strato-regions", 0)};
    u8 *guest{}, *mirror{};
    gpu::Settings settings;
    gpu::Process process;
    gpu::GPU gpu{{&process, &settings}};
    std::shared_ptr<Buffer> buffer;
    Fixture() {
        Check(fd >= 0 && !ftruncate(fd, Size), "memfd");
        guest = static_cast<u8*>(mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        mirror = static_cast<u8*>(mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        Check(guest != MAP_FAILED && mirror != MAP_FAILED, "mmap");
        std::memset(mirror, 0x11, Size);
        process.fd = fd;
        process.base = guest;
        process.trap.InstallStaticInstance();
        buffer = std::make_shared<Buffer>(gpu, span<u8>{guest, Size}, span<u8>{mirror, Size});
        buffer->SetupStagedTraps();
        buffer->SynchronizeHost();
    }
    ~Fixture() {
        buffer.reset();
        process.trap.UninstallStaticInstance();
        munmap(guest, Size);
        munmap(mirror, Size);
        close(fd);
    }
    void GpuWrite(size_t off, size_t size, const std::shared_ptr<gpu::FenceCycle>& cycle) {
        auto b = buffer;
        std::scoped_lock lock{*b};
#if HAS_BUFFER_REGIONS
        b->MarkGpuDirtyImplStaged(off, size);
#else
        (void)off;
        (void)size;
        b->MarkGpuDirtyImplStaged();
#endif
        b->cycle = cycle;
    }
    void Store(size_t off, u8 value) {
        *static_cast<volatile u8*>(guest + off) = value;
    }
    u8 Load(size_t off) {
        return *static_cast<volatile u8*>(guest + off);
    }
};
