// SPDX-License-Identifier: MPL-2.0
// Only GPU I/O/resource interfaces are facades. Methods under test are extracted
// unchanged from production; this is not an Android or Vulkan validation test.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using u32 = uint32_t;
using i32 = int32_t;
constexpr u32 VK_REMAINING_ARRAY_LAYERS = ~0U, VK_REMAINING_MIP_LEVELS = ~0U, VK_QUEUE_FAMILY_IGNORED = ~0U;
#define TRACE_EVENT(...) ((void)0)
namespace boost::container { template<class T, unsigned> using small_vector = std::vector<T>; }
template<class T> struct span {
    span(T) {}
    template<class A> span(const std::vector<T, A> &) {}
};
namespace vk {
enum class ImageLayout { eUndefined, eGeneral, eTransferSrcOptimal, eTransferDstOptimal };
enum class PipelineStageFlagBits { eTopOfPipe, eAllCommands, eTransfer };
enum class AccessFlagBits { eMemoryWrite, eMemoryRead, eTransferRead, eTransferWrite };
enum class CommandBufferUsageFlagBits { eOneTimeSubmit };
enum class Filter { eLinear };
using Semaphore = int;
struct Extent3D { u32 width{4}, height{4}, depth{1}; bool operator==(const Extent3D &) const = default; };
struct Offset3D { i32 x, y, z; };
struct ImageSubresourceRange { int aspectMask{1}; u32 baseMipLevel{}, levelCount{1}, baseArrayLayer{}, layerCount{1}; };
struct ImageSubresourceLayers { int aspectMask; u32 mipLevel, baseArrayLayer, layerCount; };
struct ImageMemoryBarrier {
    int image;
    AccessFlagBits srcAccessMask, dstAccessMask;
    ImageLayout oldLayout, newLayout;
    u32 srcQueueFamilyIndex, dstQueueFamilyIndex;
    ImageSubresourceRange subresourceRange;
};
struct ImageCopy { ImageSubresourceLayers srcSubresource, dstSubresource; Extent3D extent; };
struct ImageBlit {
    ImageSubresourceLayers srcSubresource;
    std::array<Offset3D, 2> srcOffsets;
    ImageSubresourceLayers dstSubresource;
    std::array<Offset3D, 2> dstOffsets;
};
struct CommandBufferBeginInfo { CommandBufferUsageFlagBits flags; };
namespace raii {
struct CommandBuffer {
    std::function<void()> record;
    std::function<void()> finish;
    void begin(CommandBufferBeginInfo) const {}
    void end() const { if (finish) finish(); }
    void pipelineBarrier(PipelineStageFlagBits, PipelineStageFlagBits, int, int, int, ImageMemoryBarrier) const {}
    void blitImage(int, ImageLayout, int, ImageLayout, ImageBlit, Filter) const { if (record) record(); }
    void copyImage(int, ImageLayout, int, ImageLayout, ImageCopy) const { if (record) record(); }
};
}
}

namespace skyline::gpu {
using namespace std::chrono_literals;
template<class... T> std::runtime_error exception(const char *message, T...) { return std::runtime_error(message); }
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct FenceCycle {
    std::mutex mutex;
    std::condition_variable changed;
    bool submitted{}, complete{}, cancelled{}, semaphoreConsumed{};
    unsigned waits{};
    std::vector<std::shared_ptr<void>> dependencies;
    std::shared_ptr<FenceCycle> semaphoreUnsignalCycle;
    std::function<void()> attaching;
    void WaitSubmit() { std::unique_lock lock{mutex}; changed.wait(lock, [&] { return submitted || cancelled; }); }
    void Wait() {
        std::unique_lock lock{mutex}; ++waits; changed.notify_all();
        changed.wait(lock, [&] { return complete || cancelled; });
        const auto unsignal{semaphoreUnsignalCycle}; lock.unlock();
        if (unsignal) unsignal->Wait();
    }
    void NotifySubmitted() { std::scoped_lock lock{mutex}; submitted = true; changed.notify_all(); }
    void Complete() {
        std::scoped_lock lock{mutex}; complete = true; dependencies.clear(); changed.notify_all();
    }
    void Cancel() { cancelled = true; Complete(); }
    void ChainCycle(const std::shared_ptr<FenceCycle> &) {} // Ordering itself is covered by gpu_submission.
    template<class... T> void AttachObjects(const T &...objects) {
        if (attaching) attaching();
        (dependencies.push_back(objects), ...);
    }
    std::shared_ptr<FenceCycle> RecordSemaphoreWaitUsage(std::function<std::shared_ptr<FenceCycle>(vk::Semaphore)> &&callback) {
        WaitSubmit();
        if (semaphoreConsumed) { Wait(); return callback({}); }
        semaphoreUnsignalCycle = callback(1); semaphoreConsumed = true;
        return semaphoreUnsignalCycle;
    }
};
enum class Failure { None, Staging, Allocate, Attach, Upload, RecordCopy, End, Submit };
struct Staging { int byte; };
struct Scheduler {
    Failure fail{};
    std::vector<std::shared_ptr<FenceCycle>> cycles;
    std::function<void(const std::shared_ptr<FenceCycle> &)> submitting;
    void Fail(Failure point) { if (fail == point) throw std::runtime_error("injected GPU failure"); }
    struct Active {
        vk::raii::CommandBuffer command;
        std::shared_ptr<FenceCycle> cycle;
        auto GetFenceCycle() const { return cycle; }
        auto *operator->() { return &command; }
        auto &operator*() { return command; }
    };
    Active AllocateCommandBuffer() {
        Fail(Failure::Allocate);
        auto cycle{std::make_shared<FenceCycle>()}; cycles.push_back(cycle);
        cycle->attaching = [this] { Fail(Failure::Attach); };
        return {{[this] { Fail(Failure::RecordCopy); }, [this] { Fail(Failure::End); }}, cycle};
    }
    void SubmitCommandBuffer(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &cycle,
                             span<vk::Semaphore> = 0, span<vk::Semaphore> = 0) {
        Fail(Failure::Submit);
        if (submitting) submitting(cycle);
        cycle->NotifySubmitted();
    }
    template<class F> auto Submit(F &&record, span<vk::Semaphore> waits = 0, span<vk::Semaphore> signals = 0) {
        auto active{AllocateCommandBuffer()};
        try { record(*active); active->end(); SubmitCommandBuffer(*active, active.cycle, waits, signals); }
        catch (...) { active.cycle->Cancel(); throw; }
        return active.cycle;
    }
    void Complete() { for (auto &cycle : cycles) cycle->Complete(); }
};
struct Texture : std::enable_shared_from_this<Texture> {
    enum class DirtyState { CpuDirty, Clean, GpuDirty };
    struct Group { std::recursive_mutex operationMutex; };
    struct AtomicGroup { std::shared_ptr<Group> value; auto load() const { return value; } } aliasGroup;
    std::atomic<bool> aliasReadOnly{};
    DirtyState dirtyState{DirtyState::CpuDirty};
    std::recursive_mutex stateMutex;
    struct Trap { void TrapRegions(int, bool) {} };
    struct Process { Trap trap; };
    struct Settings { std::shared_ptr<bool> freeGuestTextureMemory{std::make_shared<bool>(true)}; };
    struct GPU {
        struct State { std::shared_ptr<Settings> settings{std::make_shared<Settings>()}; std::shared_ptr<Process> process{std::make_shared<Process>()}; } state;
        Scheduler scheduler;
    } &gpu;
    bool guest{true}, everUsedAsGpuWrite{true};
    std::shared_ptr<int> trapHandle{std::make_shared<int>(0)};
    std::shared_ptr<FenceCycle> cycle;
    vk::ImageLayout layout{vk::ImageLayout::eGeneral};
    vk::Extent3D dimensions;
    u32 layerCount{1}, levelCount{1};
    int format{1}, guestByte{42}, hostByte{7};
    unsigned stages{}, discards{};
    bool mapped{};
    std::function<void()> stagingHook;
    explicit Texture(GPU &gpu) : gpu{gpu} {}
    bool HasAliases() const { return bool(aliasGroup.load()); }
    void FreeGuest() { if (*gpu.state.settings->freeGuestTextureMemory) { ++discards; guestByte = 0; } }
    void WaitOnBacking() {}
    int GetBacking() const { return 1; }
    void WaitOnFence();
    auto SynchronizeHostImpl() {
        ++stages; gpu.scheduler.Fail(Failure::Staging);
        if (stagingHook) stagingHook();
        if (mapped) { WaitOnFence(); hostByte = guestByte; return std::shared_ptr<Staging>{}; }
        return std::make_shared<Staging>(Staging{guestByte});
    }
    void CopyFromStagingBuffer(const vk::raii::CommandBuffer &, const std::shared_ptr<Staging> &staging) {
        layout = vk::ImageLayout::eGeneral;
        gpu.scheduler.Fail(Failure::Upload); hostByte = staging->byte;
    }
    void SynchronizeHost(bool gpuDirty = false);
    void SynchronizeHostInline(const vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, bool gpuDirty);
    void CopyFrom(std::shared_ptr<Texture>, vk::Semaphore, vk::Semaphore, int, const vk::ImageSubresourceRange &);
};
namespace texture { using Format = int; }
#include "production.inc"

void Uploads() {
    for (bool inlineUpload : {false, true}) for (bool eviction : {false, true})
    for (bool gpuDirty : {false, true}) for (auto failure : {Failure::Staging, Failure::Upload,
                                                          Failure::Allocate, Failure::Attach, Failure::End, Failure::Submit}) {
        if (inlineUpload && failure != Failure::Staging && failure != Failure::Upload && failure != Failure::Attach) continue;
        Texture::GPU gpu; *gpu.state.settings->freeGuestTextureMemory = eviction;
        gpu.scheduler.submitting = [](const auto &cycle) {
            Check(cycle->dependencies.size() == 2, "upload submitted before retaining its lifetimes");
        };
        auto t{std::make_shared<Texture>(gpu)};
        auto prior{std::make_shared<FenceCycle>()}; prior->NotifySubmitted(); t->cycle = prior;
        auto current{std::make_shared<FenceCycle>()}; current->attaching = [&] { gpu.scheduler.Fail(Failure::Attach); };
        vk::raii::CommandBuffer command;
        t->layout = vk::ImageLayout::eUndefined;
        auto upload = [&] { if (inlineUpload) t->SynchronizeHostInline(command, current, gpuDirty); else t->SynchronizeHost(gpuDirty); };
        gpu.scheduler.fail = failure;
        bool failed{}; try { upload(); } catch (const std::runtime_error &) { failed = true; }
        Check(failed, "failure injection did not execute");
        Check(t->dirtyState == Texture::DirtyState::CpuDirty, "failed upload lost CPU authority");
        Check(t->guestByte == 42 && !t->discards && t->cycle == prior, "failed upload published/discarded data");
        if (!inlineUpload) Check(t->layout == vk::ImageLayout::eUndefined, "failed standalone upload published layout");
        gpu.scheduler.fail = Failure::None;
        const auto attempts{t->stages}; upload();
        Check(t->stages == attempts + 1 && t->hostByte == 42, "retry skipped the CPU upload");
        Check(t->discards == unsigned(eviction && gpuDirty), "wrong successful upload discard policy");
        gpu.scheduler.Complete(); prior->Complete(); current->Complete();
    }
    // A CPU write after write protection is armed must remain dirty on success.
    for (bool inlineUpload : {false, true}) {
        Texture::GPU gpu; auto t{std::make_shared<Texture>(gpu)};
        auto current{std::make_shared<FenceCycle>()}; vk::raii::CommandBuffer command;
        t->stagingHook = [&] { t->dirtyState = Texture::DirtyState::CpuDirty; };
        if (inlineUpload) t->SynchronizeHostInline(command, current, false); else t->SynchronizeHost(false);
        Check(t->dirtyState == Texture::DirtyState::CpuDirty && !t->discards, "staging race lost a CPU write");
        gpu.scheduler.Complete(); current->Complete();
    }
    std::puts("PASS: production upload failure/retry, eviction OFF/ON and CPU write during staging");
}


}

int main(int argc, char **argv) {
    try {
        if (argc == 1 || std::string{argv[1]} == "uploads") skyline::gpu::Uploads();
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
