// SPDX-License-Identifier: MPL-2.0
// Run the unchanged production method/callbacks; only GPU I/O is replaced.
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <variant>
#include <gpu/texture/overlap_group.h>
#define TRACE_EVENT(...) ((void)0)
namespace vk {
enum class ImageLayout { eUndefined, eGeneral };
enum class ImageTiling { eOptimal, eLinear, eUnsupported };
inline const char *to_string(ImageTiling) { return "unsupported"; }
namespace raii { struct CommandBuffer {}; }
}
namespace skyline::gpu {
using i64 = long long;
namespace util { inline i64 GetTimeNs() { return 0; } }
template<class... Args> std::runtime_error exception(const char *message, Args...) { return std::runtime_error(message); }
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
enum class Failure { None, Allocate, PriorFence, Record, Submit, DownloadFence, Copy };
struct Io {
    Failure failure{};
    unsigned copies{}, waits{};
    std::function<void()> beforeCopy;
    void Fail(Failure point) const { if (failure == point) throw std::runtime_error("injected GPU I/O failure"); }
};
namespace memory {
struct Image { int value{42}; int *data() { return &value; } };
struct StagingBuffer { int value{42}; int *data() { return &value; } };
}
struct FenceCycle {
    Io &io;
    void Wait() { ++io.waits; io.Fail(Failure::DownloadFence); }
};
struct Trap {
    std::mutex mutex;
    unsigned deletes{}, retraps{};
    void RemoveTrap(int) { throw std::logic_error("Partial path uses real traps in the overlap suite"); }
    void DeleteTrap(int) { std::scoped_lock lock{mutex}; ++deletes; }
    void TrapRegions(int, bool writeOnly) { std::scoped_lock lock{mutex}; Check(writeOnly, "unexpected read trap rearm"); ++retraps; }
};
struct GPU {
    Io io;
    struct Process { Trap trap; };
    struct Settings { std::shared_ptr<bool> enableFastGpuReadbackHack{std::make_shared<bool>(false)}; };
    struct { std::shared_ptr<Process> process{std::make_shared<Process>()}; std::shared_ptr<Settings> settings{std::make_shared<Settings>()}; } state;
    struct Memory {
        Io &io;
        std::shared_ptr<memory::StagingBuffer> AllocateStagingBuffer(size_t) { io.Fail(Failure::Allocate); return std::make_shared<memory::StagingBuffer>(); }
    } memory{io};
    struct Scheduler {
        Io &io;
        template<class F> auto Submit(F &&record) {
            vk::raii::CommandBuffer buffer;
            record(buffer);
            io.Fail(Failure::Submit);
            return std::make_shared<FenceCycle>(FenceCycle{io});
        }
    } scheduler{io};
};
class Texture : public std::enable_shared_from_this<Texture> {
    std::recursive_mutex resourceMutex;
public:
    enum class DirtyState { Clean, CpuDirty, GpuDirty };
    GPU &gpu;
    struct Guest { int format{1}; };
    std::optional<Guest> guest{Guest{}};
    int format{1}, guestByte{7};
    vk::ImageLayout layout{vk::ImageLayout::eGeneral};
    vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
    std::variant<int, memory::Image> backing{0};
    std::shared_ptr<memory::StagingBuffer> downloadStagingBuffer;
    size_t surfaceSize{4};
    std::optional<int> trapHandle{1};
    mutable std::recursive_mutex stateMutex;
    texture::AtomicSharedPtr<texture::AliasGroup> aliasGroup;
    std::atomic<bool> aliasStale{}, aliasReadOnly{};
    DirtyState dirtyState{DirtyState::GpuDirty};
    bool memoryFreed{true};
    bool everUsedAsGpuWrite{true};
    uint64_t gpuWriteSerial{9}, gpuReadbackSerial{3};
    std::shared_ptr<FenceCycle> cycle;
    static constexpr std::chrono::nanoseconds SkipReadbackHackWaitTimeThreshold{250};
    static constexpr size_t SkipReadbackHackWaitCountThreshold{6};
    std::chrono::nanoseconds accumulatedGuestWaitTime{};
    size_t accumulatedGuestWaitCounter{};
    explicit Texture(GPU &gpu) : gpu{gpu} {}
    void lock() { resourceMutex.lock(); }
    bool try_lock() { return resourceMutex.try_lock(); }
    void unlock() { resourceMutex.unlock(); }
    std::shared_ptr<texture::OverlapState> overlapState;
    bool HasPendingOverlapWrite() const { return false; }
    bool HasOverlaps() const { return false; } // This suite covers the unchanged ungrouped/exact-alias paths.
    bool HasAliases() const { return bool(aliasGroup.load()); }
    void WaitOnBacking() {}
    void WaitOnFence() { gpu.io.Fail(Failure::PriorFence); }
    void CopyIntoStagingBuffer(vk::raii::CommandBuffer &, const std::shared_ptr<memory::StagingBuffer> &) { gpu.io.Fail(Failure::Record); }
    void CopyToGuest(int *data) {
        if (gpu.io.beforeCopy) gpu.io.beforeCopy();
        gpu.io.Fail(Failure::Copy);
        guestByte = *data;
        ++gpu.io.copies;
    }
    void SynchronizeGuest(bool cpuDirty = false, bool skipTrap = false);
    auto LockCallback() { std::weak_ptr<Texture> weakThis{shared_from_this()}; return
#include "lock.inc"
    ; }
    auto ReadTrap() { std::weak_ptr<Texture> weakThis{shared_from_this()}; return
#include "read.inc"
    ; }
    auto WriteTrap() { std::weak_ptr<Texture> weakThis{shared_from_this()}; return
#include "write.inc"
    ; }
};
#include "sync.inc"
void Run() {
    for (bool cpuDirty : {false, true}) for (bool skipTrap : {false, true}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        gpu.io.beforeCopy = [&] {
            Check(t->dirtyState == Texture::DirtyState::GpuDirty && t->memoryFreed && t->gpuReadbackSerial == 3,
                  "guest validity was published before the readback copy");
        };
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(cpuDirty, skipTrap); }
        Check(t->guestByte == 42 && !t->memoryFreed && t->gpuReadbackSerial == 9, "successful readback not published");
        Check(t->dirtyState == (cpuDirty ? Texture::DirtyState::CpuDirty : Texture::DirtyState::Clean), "wrong final state");
        Check(gpu.state.process->trap.deletes == unsigned(cpuDirty && !skipTrap) &&
              gpu.state.process->trap.retraps == unsigned(!cpuDirty && !skipTrap), "wrong final protection");
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(cpuDirty, skipTrap); }
        Check(gpu.io.copies == 1 && gpu.io.waits == 1, "unnecessary repeat readback");
    }
    for (bool grouped : {false, true}) for (auto failure : {Failure::Allocate, Failure::PriorFence, Failure::Record, Failure::Submit, Failure::DownloadFence, Failure::Copy}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        auto group{std::make_shared<texture::AliasGroup>()}; group->generation = 8; group->guestGeneration = 2; group->writerIdentity = t.get();
        if (grouped) t->aliasGroup.store(group);
        gpu.io.failure = failure;
        bool failed{};
        try { std::scoped_lock lock{*t}; t->SynchronizeGuest(true, false); } catch (const std::runtime_error &) { failed = true; }
        Check(failed && t->dirtyState == Texture::DirtyState::GpuDirty && t->memoryFreed && t->gpuReadbackSerial == 3,
              "failed readback published validity");
        Check(group->guestGeneration == 2 && !gpu.state.process->trap.deletes && !gpu.state.process->trap.retraps, "failed readback changed generation/protection");
        gpu.io.failure = Failure::None;
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(false, true); }
        Check(gpu.io.copies == 1 && (!grouped || group->guestGeneration == 8), "retry did not preserve alias writer");
    }
    for (bool write : {false, true}) for (bool freed : {false, true}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        t->memoryFreed = freed;
        *gpu.state.settings->enableFastGpuReadbackHack = true;
        t->accumulatedGuestWaitTime = std::chrono::nanoseconds{500};
        // A non-trap caller pauses inside the guest copy, with the texture locked.
        std::promise<void> entered, release; auto enteredFuture{entered.get_future()}; auto releaseFuture{release.get_future()};
        gpu.io.beforeCopy = [&] { entered.set_value(); releaseFuture.wait(); };
        auto download = std::async(std::launch::async, [&] { std::scoped_lock lock{*t}; t->SynchronizeGuest(false, false); });
        enteredFuture.wait();
        bool permitted;
        {
            // HandleTrap owns trapMutex only for the nonblocking callback.
            std::scoped_lock trapLock{gpu.state.process->trap.mutex};
            permitted = write ? t->WriteTrap()() : t->ReadTrap()();
        }
        // Production lockCallback may wait, and therefore runs outside trapMutex.
        auto retry = std::async(std::launch::async, t->LockCallback());
        release.set_value(); download.get(); retry.get();
        Check(!permitted, "trap released guest access while readback was pending");
        Check(write ? t->WriteTrap()() : t->ReadTrap()(), "trap could not retry after completed readback");
        Check(t->guestByte == 42 && gpu.io.copies == 1, "trap retry duplicated readback or exposed old bytes");
    }
    for (bool mismatch : {false, true}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        if (mismatch) t->format = 2; else t->layout = vk::ImageLayout::eUndefined;
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(false, true); }
        Check(t->dirtyState == Texture::DirtyState::GpuDirty && t->memoryFreed && t->gpuReadbackSerial == 3,
              "skipped readback published validity");
        for (bool write : {false, true}) {
            bool failed{};
            try { (void)(write ? t->WriteTrap()() : t->ReadTrap()()); } catch (const std::runtime_error &) { failed = true; }
            Check(failed && t->dirtyState == Texture::DirtyState::GpuDirty && t->memoryFreed,
                  "unavailable readback silently released a CPU trap");
        }
    }
    for (bool cpuDirty : {false, true}) for (bool skipTrap : {false, true}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        // Host-converted sampled-only images cannot encode back, but FreeGuest
        // preserved their original bytes and no GPU write has superseded them.
        t->format = 2; t->memoryFreed = false; t->everUsedAsGpuWrite = false;
        t->gpuWriteSerial = 0; t->gpuReadbackSerial = 0;
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(cpuDirty, skipTrap); }
        Check(t->dirtyState == (cpuDirty ? Texture::DirtyState::CpuDirty : Texture::DirtyState::Clean) && t->guestByte == 7,
              "converted sampled-only image lost its valid retained guest representation");
        Check(t->gpuReadbackSerial == 0 && !gpu.io.waits && !gpu.io.copies, "retained guest handling fabricated a GPU readback");
        Check(gpu.state.process->trap.deletes == unsigned(cpuDirty && !skipTrap) &&
              gpu.state.process->trap.retraps == unsigned(!cpuDirty && !skipTrap), "retained guest protection was not restored");
    }
    for (unsigned unsafeCase : {0U, 1U, 2U}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        t->format = 2; t->memoryFreed = unsafeCase == 0; t->everUsedAsGpuWrite = unsafeCase == 1;
        if (unsafeCase == 2) t->aliasGroup.store(std::make_shared<texture::AliasGroup>());
        t->SynchronizeGuest(true, false);
        Check(t->dirtyState == Texture::DirtyState::GpuDirty && t->gpuReadbackSerial == 3 &&
              t->guestByte == 7 && !gpu.state.process->trap.deletes,
              "retained-guest exception accepted an evicted, written or grouped image");
    }
    for (bool stale : {false, true}) {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        if (stale) t->aliasStale = true; else t->aliasReadOnly = true;
        t->SynchronizeGuest();
        Check(!gpu.io.copies && !gpu.io.waits && t->gpuReadbackSerial == 3 && t->memoryFreed, "inactive alias became an encoding source");
    }
    {
        GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        t->tiling = vk::ImageTiling::eLinear; t->backing = memory::Image{};
        { std::scoped_lock lock{*t}; t->SynchronizeGuest(false, false); }
        Check(t->guestByte == 42 && t->dirtyState == Texture::DirtyState::Clean && !t->memoryFreed && gpu.io.waits == 0, "mapped readback publication failed");
    }
    std::cout << "PASS: readback publication, failure/retry, concurrent trap rejection, alias guards, mapped image and repeat-use paths\n";
}
}
int main() { try { skyline::gpu::Run(); } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; } }
