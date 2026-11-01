// SPDX-License-Identifier: MPL-2.0
// Host interfaces around unmodified production Buffer callbacks and sync methods.
#include "runtime.h"
#include <optional>
#include <ucontext.h>
#include <fcntl.h>
#include <thread>
#include <future>
namespace skyline::gpu {
    struct FenceCycle {
        unsigned waits{};
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
        void unlock() { mutex.unlock(); }
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
static void Pipeline() {
    for (bool old : {true, false}) {
        Fixture f(old);
        auto cycle = f.Pending(true);
        Check(f.Read() == 0x55, "mandatory GPU readback returned stale data");
        Check(cycle->waits == 1, "mandatory fence was skipped or waited twice");
        auto seq = f.buffer->sequence;
        {
            std::scoped_lock lock{*f.buffer};
            f.buffer->SynchronizeHost();
        }
        const auto uploads = f.buffer->sequence - seq;
        Check(uploads == (old ? 1U : 0U), "read-only access uploaded unexpectedly");
        Check(std::memcmp(f.mirror, f.buffer->backing->data(), 4096) == 0, "roundtrip mismatch");
        std::printf("%s GPU-write/CPU-read/GPU-read: mandatory readback=4096B waits=%u false upload=%uB\n",
                    old ? "A safe legacy" : "C safe precise", cycle->waits, uploads * 4096);
    }
    for (bool old : {true, false}) {
        Fixture f(old);
        auto cycle = f.Pending(false);
        // A lone Clean buffer normally traps writes only. Another resource on
        // its page can impose PROT_NONE and make this otherwise valid load fault.
        f.process.trap.TrapRegions(*f.buffer->trapHandle, true);
        std::array<span<u8>, 1> collateral{span<u8>{f.guest + 2048, 128}};
        auto other = f.process.trap.CreateTrap(collateral, [] {}, [] { return true; }, [] { return true; });
        f.process.trap.TrapRegions(other, false);
        Check(f.Read() == 0x55, "valid mirror read failed");
        Check(cycle->waits == (old ? 1U : 0U), "independent CPU load blocked on GPU reader");
        std::printf("%s collateral clean mirror/GPU-reader: unrelated waits=%u\n", old ? "A safe legacy" : "C safe precise",
                    cycle->waits);
        if (!old) {
            f.Write(37, 0x9a);
            Check(cycle->waits == 1, "real CPU write failed to wait for GPU reader");
        }
        f.process.trap.DeleteTrap(other);
    }
    {
        Fixture f(true, true);
        auto cycle = f.Pending(true);
        Check(f.Read() == 0x77 && cycle->waits == 0, "hack control did not reproduce unsynchronized stale read");
        std::puts("B unsafe hack: 0 waits, stale sentinel observed (expected negative control)");
    }
    for (bool whole : {false, true}) {
        Fixture f;
        auto cycle = f.Pending(true);
        Check(f.Read() == 0x55, "readback mismatch");
        if (whole)
            for (size_t i{}; i < 4096; ++i)
                f.Write(i, 0x9a);
        else
            f.Write(37, 0x9a);
        Check(signalReads == 1 && signalWrites == 1, "load then store did not fault exactly once each");
        Check(f.buffer->dirtyState == Buffer::DirtyState::CpuDirty, "store not tracked after read");
        {
            std::scoped_lock lock{*f.buffer};
            f.buffer->SynchronizeHost();
        }
        for (size_t i{}; i < 4096; ++i)
            Check(f.buffer->backing->data()[i] == (whole || i == 37 ? 0x9a : 0x55),
                  "partial/full upload corrupted preserved bytes");
        Check(f.Read(19) == (whole ? 0x9a : 0x55), "read after upload mismatch");
        f.Write(19, 0xa7);
        Check(signalWrites == 2, "rearmed store not trapped");
    }
    {
        Fixture f;
        f.Pending(true);
        f.buffer->Invalidate();
        Check(!f.buffer->SynchronizeGuest(), "invalidated backing remained synchronizable");
        f.Write(0, 0x39);
        Check(f.Read() == 0x39, "invalidated trap survived");
    }
    {
        Fixture f;
        auto first = f.Pending(true);
        auto second = std::make_shared<gpu::FenceCycle>();
        second->complete = [&] { std::memset(f.buffer->backing->data(), 0x64, 4096); };
        first->complete = [&] { f.buffer->cycle = second; };
        Check(f.Read() == 0x64 && first->waits == 1 && second->waits == 1,
              "replaced pending cycle not waited before consumption");
    }
    {
        Fixture f;
        f.Pending(true);
        auto weak = std::weak_ptr(f.buffer);
        f.buffer.reset();
        Check(weak.expired(), "trap prolonged buffer lifetime");
        f.Write(0, 0x39);
        Check(f.Read() == 0x39, "expired resource trap survived");
    }
    {
        Fixture f;
        std::memset(f.buffer->backing->data(), 0x55, 4096);
        f.buffer->dirtyState = Buffer::DirtyState::GpuDirty;
        f.process.trap.TrapRegions(*f.buffer->trapHandle, false);
        std::promise<void> held;
        std::thread owner([&] {
            std::scoped_lock resource{f.buffer->mutex};
            held.set_value();
            f.buffer->lockCalls.wait(0);
            // Must be possible while the callback is waiting for the resource.
            f.process.trap.TrapRegions(*f.buffer->trapHandle, false);
        });
        held.get_future().wait();
        auto value = f.Read();
        owner.join();
        Check(value == 0x55, "contended read consumed stale bytes");
    }
    std::puts("PASS actual CPU page faults + production Buffer callbacks/sync: mandatory waits, clean-read "
              "independence, partial/full writes, rearm, invalidation");
}
#ifdef WITH_VULKAN
#include "vulkan.inc"
#endif
int main() {
    struct sigaction action {};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    Check(!sigaction(SIGSEGV, &action, nullptr), "sigaction failed");
    try {
        Pipeline();
#ifdef WITH_VULKAN
        VulkanPipeline();
#endif
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
