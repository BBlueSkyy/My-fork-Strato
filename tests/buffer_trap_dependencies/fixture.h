// SPDX-License-Identifier: MPL-2.0
// Host interfaces around unmodified production Buffer callbacks and sync methods.
#include "runtime.h"
#include <optional>
#include <ucontext.h>
#include <fcntl.h>
#include <thread>
#include <future>
namespace skyline {using ContextTag=uint64_t;}
namespace skyline::gpu {
    struct FenceCycle {
        std::atomic<unsigned> waits{};
        bool done{};
        std::function<void()> complete;
        void Wait() {
            ++waits;
            if (!done && complete)
                complete();
            done = true;
        }
        bool Poll() { return done; }
    };
    struct Backing {
        std::vector<u8> bytes = std::vector<u8>(4096);
        u8 *external{};
        u8 *data() { return external ? external : bytes.data(); }
    };
    struct Settings {
        std::shared_ptr<bool> enableFastGpuReadbackHack = std::make_shared<bool>(false),
                              enableFastReadbackWrites = std::make_shared<bool>(true);
    };
    struct Process {
        TrapManager trap;
    };
    struct GPU {
        struct {
            Process *process;
            Settings *settings;
        } state;
    };
    struct Buffer : std::enable_shared_from_this<Buffer> {
        enum class DirtyState { Clean, CpuDirty, GpuDirty };
        DirtyState dirtyState{DirtyState::Clean};
        GPU &gpu;
        const size_t id{1};
        std::optional<span<u8>> guest;
        span<u8> mirror;
        std::shared_ptr<Backing> backing = std::make_shared<Backing>();
        std::optional<TrapHandle> trapHandle;
        std::shared_ptr<FenceCycle> cycle;
        std::recursive_mutex stateMutex;
        std::mutex mutex;
        bool isDirect{}, blocked{};
        unsigned sequence{};
        std::atomic<unsigned> lockCalls{};
        static constexpr size_t FastReadbackHackWaitCountThreshold{6};
        static constexpr auto FastReadbackHackWaitTimeThreshold = std::chrono::nanoseconds(250000000);
        std::chrono::nanoseconds accumulatedGuestWaitTime{};
        size_t accumulatedGuestWaitCounter{};
        Buffer(GPU &gpu, span<u8> guest, span<u8> mirror) : gpu(gpu), guest(guest), mirror(mirror) {}
        ~Buffer() { Invalidate(); }
        void lock() {
            ++lockCalls;
            lockCalls.notify_all();
            mutex.lock();
        }
        void unlock();
        uint64_t tag{};bool currentExecutionGpuDirty{};
        void AllowAllBackingWrites(){std::scoped_lock lock{stateMutex};blocked=false;}
        bool try_lock() { return mutex.try_lock(); }
        bool AllCpuBackingWritesBlocked() { return blocked; }
        void BlockAllCpuBackingWrites() { blocked = true; }
        void AdvanceSequence() { ++sequence; }
        void SetupStagedTraps();
        void WaitOnFence();
        bool PollFence();
        void Invalidate();
        void SynchronizeHost(bool skipTrap = false);
        bool SynchronizeGuest(bool skipTrap = false, bool nonBlocking = false);
        void MarkGpuDirtyImplStaged();
    };
} // namespace skyline::gpu
#include "buffer.inc"
using skyline::gpu::Buffer;
static bool legacy{};
static unsigned signalReads{}, signalWrites{};
static void Fault(int signal, siginfo_t *info, void *raw) {
    auto *native = static_cast<ucontext_t *>(raw);
#if defined(__x86_64__)
    const bool write = native->uc_mcontext.gregs[REG_ERR] & 2;
#else
#error "This host fixture translates x86 page-fault metadata into synthetic arm64 ESR"
#endif
    if (write)
        ++signalWrites;
    else
        ++signalReads;
    ArmContext ctx;
    uint32_t magic = 0x45535201, size = 16;
    uint64_t esr = (0x24ULL << 26) | (write ? 0x0fULL : 0x07ULL) | (write ? 64 : 0);
    std::memcpy(ctx.uc_mcontext.__reserved, &magic, 4);
    std::memcpy(ctx.uc_mcontext.__reserved + 4, &size, 4);
    std::memcpy(ctx.uc_mcontext.__reserved + 8, &esr, 8);

    if (!(legacy ? TrapManager::TrapHandler(static_cast<u8 *>(info->si_addr), true) : RouteGuest(signal, info, &ctx)))
        _exit(90);
}
struct Fixture {
    gpu::Process process;
    gpu::Settings settings;
    gpu::GPU gpu{{&process, &settings}};
    int fd = memfd_create("strato-trap", 0);
    u8 *guest{}, *mirror{};
    std::shared_ptr<Buffer> buffer;
    Fixture(bool old = false, bool hack = false) {
        legacy = old;
        signalReads = signalWrites = 0;
        Check(fd >= 0 && !ftruncate(fd, 4096), "shared guest allocation failed");
        guest = static_cast<u8 *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        mirror = static_cast<u8 *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        Check(guest != MAP_FAILED && mirror != MAP_FAILED, "mirror map failed");
        std::memset(mirror, 0x11, 4096);
        process.trap.InstallStaticInstance();
        buffer = std::make_shared<Buffer>(gpu, span<u8>{guest, 4096}, span<u8>{mirror, 4096});
        buffer->SetupStagedTraps();
        *settings.enableFastGpuReadbackHack = hack;
        if (hack)
            buffer->accumulatedGuestWaitTime = std::chrono::seconds(1);
    }
    ~Fixture() {
        buffer.reset();
        process.trap.UninstallStaticInstance();
        munmap(guest, 4096);
        munmap(mirror, 4096);
        close(fd);
    }
    u8 Read(size_t offset = 0) { return *static_cast<volatile u8 *>(guest + offset); }
    void Write(size_t offset, u8 value) { *static_cast<volatile u8 *>(guest + offset) = value; }
    std::shared_ptr<gpu::FenceCycle> Pending(bool writes) {
        auto cycle = std::make_shared<gpu::FenceCycle>();
        buffer->cycle = cycle;
        buffer->blocked = true;
        if (writes) {
            buffer->dirtyState = Buffer::DirtyState::GpuDirty;
            cycle->complete = [this] { std::memset(buffer->backing->data(), 0x55, 4096); };
        } else
            std::memset(buffer->mirror.data(), 0x55, 4096);
        std::memset(buffer->backing->data(), 0x77, 4096);
        process.trap.TrapRegions(*buffer->trapHandle, false);
        return cycle;
    }
};
