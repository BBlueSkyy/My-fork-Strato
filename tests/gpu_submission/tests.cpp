#include <gpu/fence_cycle.h>
#include <gpu/buffer_region_state.h>
namespace vk {
    using DeviceSize = uint64_t;
}
using skyline::gpu::BufferRegionState;
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <list>
#include <vector>
#include <cstring>

using skyline::gpu::FenceCycle;
using namespace std::chrono_literals;
static int failures;
static void check(bool success, const char *message) {
    std::cout << (success ? "PASS " : "FAIL ") << message << '\n';
    if (!success) ++failures;
}
static bool ready(std::future<void> &future) { return future.wait_for(250ms) == std::future_status::ready; }

// A future gate proves Wait is inside vkWaitForFences before starting the
// submission-only waiter. The timeout observes progress; it never opens a gate.
static void submittedWaitIgnoresGpuCompletion() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    cycle.NotifySubmitted();
    auto entered = device.enteredFuture();
    auto gpuWait = std::async(std::launch::async, [&] { cycle.Wait(); });
    entered.get();
    auto submitWait = std::async(std::launch::async, [&] { cycle.WaitSubmit(); });
    check(ready(submitWait), "WaitSubmit returns while another thread waits on unsignalled GPU fence");
    auto notification = std::async(std::launch::async, [&] { cycle.NotifySubmitted(); });
    check(ready(notification), "submission notification never acquires the GPU completion mutex");
    device.signal(); gpuWait.get(); submitWait.get(); notification.get();
}

static void submissionRacingGpuWaitDoesNotTakeCompletionMutex() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    std::promise<void> traversed, proceed;
    auto proceedFuture = proceed.get_future();
    auto submitWait = std::async(std::launch::async, [&] {
        skyline::submission_test::afterIteration = [&] { traversed.set_value(); proceedFuture.get(); };
        cycle.WaitSubmit();
    });
    // WaitSubmit has observed pending submission. Pause just before its final
    // submission-mutex acquisition, then submit and let Wait acquire the GPU
    // completion mutex. An atomic submitted fast path alone cannot pass this.
    traversed.get_future().get();
    cycle.NotifySubmitted();
    auto entered = device.enteredFuture();
    auto gpuWait = std::async(std::launch::async, [&] { cycle.Wait(); });
    entered.get();
    proceed.set_value();
    check(ready(submitWait), "pending-to-submitted race stays independent of GPU completion mutex");
    device.signal(); gpuWait.get(); submitWait.get();
}

static void cancellationWakesCompletionWaitBeforeSubmission() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    std::promise<void> started;
    auto waiter = std::async(std::launch::async, [&] { started.set_value(); cycle.Wait(); });
    started.get_future().get();
    check(waiter.wait_for(20ms) == std::future_status::timeout, "completion wait remains pending before submission");
    cycle.Cancel();
    check(ready(waiter), "Cancel releases Wait before submission without entering Vulkan");
    cycle.NotifySubmitted(); waiter.get();
    check(device.getFenceStatus({}, device) == vk::Result::eNotReady, "cancelled completion wait does not signal Vulkan fence");
}

static void cancellationWakesSubmissionWait() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    std::promise<void> started;
    auto waiter = std::async(std::launch::async, [&] { started.set_value(); cycle.WaitSubmit(); });
    started.get_future().get();
    // A bounded readiness check allows the waiter to settle without sleeping.
    check(waiter.wait_for(20ms) == std::future_status::timeout, "pending cycle waits for submission");
    cycle.Cancel();
    check(ready(waiter), "Cancel releases a pending WaitSubmit");
    cycle.NotifySubmitted(); waiter.get();
    check(device.getFenceStatus({}, device) == vk::Result::eNotReady, "Cancel does not signal Vulkan fence");
}

// Only the resource and command recording APIs are facades. The whole
// production SubmitInternal function is included below, never copied here.
namespace boost::container { template<class T, unsigned> using small_vector = std::vector<T>; }
namespace ranges {
using std::ranges::find;
namespace views {
template<class T> auto concat(const std::vector<T> &a, const std::vector<T> &b) {
    auto result = a; result.insert(result.end(), b.begin(), b.end()); return result;
}
}
}
namespace texture { enum class RenderPassUsage { None }; }
// Keep dirty-state transitions, staging lifetime attachments and cycle
// replacement in the exact production SynchronizeHostInline implementation.
#define TRACE_EVENT(...)
struct Texture : std::enable_shared_from_this<Texture> {
    std::shared_ptr<FenceCycle> cycle;
    bool staged;
    std::function<void()> onSync;
    struct Settings { std::shared_ptr<bool> freeGuestTextureMemory{std::make_shared<bool>(true)}; };
    struct Trap { void TrapRegions(int, bool) {} };
    struct Process { Trap trap; };
    struct State { std::shared_ptr<Settings> settings{std::make_shared<Settings>()}; std::shared_ptr<Process> process{std::make_shared<Process>()}; };
    struct Gpu { State state; } gpu;
    bool guest{true}, everUsedAsGpuWrite{true};
    std::atomic<bool> aliasReadOnly{};
    std::mutex stateMutex;
    enum class DirtyState { CpuDirty, GpuDirty, Clean } dirtyState{DirtyState::CpuDirty};
    std::shared_ptr<int> trapHandle{std::make_shared<int>(0)};
    Texture(std::shared_ptr<FenceCycle> cycle, bool staged, std::function<void()> onSync)
        : cycle{std::move(cycle)}, staged{staged}, onSync{std::move(onSync)} {}
    bool HasAliases() const { return false; }
    void FreeGuest() {}
    std::shared_ptr<int> SynchronizeHostImpl() { onSync(); return staged ? std::make_shared<int>(0) : nullptr; }
    void CopyFromStagingBuffer(int, const std::shared_ptr<int> &) {}
    void SynchronizeHostInline(const vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, bool);
    void UpdateRenderPassUsage(int, texture::RenderPassUsage) {}
};
using skyline::exception;
#include "texture.inc"
struct Buffer {
    std::shared_ptr<FenceCycle> cycle;
    bool guest{true}, isDirect{}, currentExecutionGpuDirty{};
    bool NeedsStagedReadbackBarrier() const {
        return !isDirect && currentExecutionGpuDirty;
    }
    enum class DirtyState { CpuDirty, Clean, GpuDirty } dirtyState{DirtyState::Clean};
    std::recursive_mutex stateMutex;
    Texture::Gpu gpu;
    std::shared_ptr<int> trapHandle{std::make_shared<int>(0)};
    std::shared_ptr<std::vector<int>> backing{std::make_shared<std::vector<int>>()};
    std::vector<int> mirror;
    bool RequiresCycleAttach() const { return true; }
    BufferRegionState stagedRegions;
    void SynchronizeHost(bool skipTrap = false);
    void SynchronizeHostRegion(bool, vk::DeviceSize, vk::DeviceSize);
    void SynchronizeHostForGpu();
    void AttachStagedProducerCycle(const std::shared_ptr<FenceCycle>& producer);
    void PrepareStagedRange(vk::DeviceSize offset, vk::DeviceSize size) {
        stagedRegions.EnsureBounds(offset, size);
    }
    void ProtectStagedRange(size_t, size_t, bool) {}
    void RefreshStagedDirtyState() {}
    void WaitOnFence();
    void AdvanceSequence() {}
    #include "buffer_cycle.inc"
    void AllowAllBackingWrites() {}
};
#include "buffer.inc"
struct BufferView {
    std::shared_ptr<Buffer> buffer;
    Buffer *operator->() const { return buffer.get(); }
};
struct Slot {
    std::list<int> nodes, pendingPostRenderPassNodes;
    int commandBuffer{};
    void WaitReady() {}
};
static void RecordFullBarrier(int) {}
static unsigned hostReadBarriers{};
[[maybe_unused]] static void RecordStagedReadbackBarrier(int&) {
    ++hostReadBarriers;
}
struct GPU {};
struct CommandExecutor {
    bool renderPass{};
    Slot storage;
    Slot *slot{&storage};
    std::shared_ptr<FenceCycle> cycle;
    std::vector<std::shared_ptr<Texture>> attachedTextures, preserveAttachedTextures;
    std::vector<BufferView> attachedBuffers, preserveAttachedBuffers;
    void FinishRenderPass() {}
    void RotateRecordSlot() {}
    void SubmitInternal();
    template <class F> void AddOutsideRpCommand(F f) {
        GPU gpu;
        f(slot->commandBuffer, cycle, gpu);
    }
};
#include "executor.inc"

static void stagedUploadWaitsForPriorSubmission() {
    vk::raii::Device oldDevice, newDevice;
    auto previous = std::make_shared<FenceCycle>(oldDevice, vk::Fence{}, vk::Semaphore{});
    auto current = std::make_shared<FenceCycle>(newDevice, vk::Fence{}, vk::Semaphore{});
    CommandExecutor executor; executor.cycle = current;
    std::promise<void> synchronized;
    auto syncFuture = synchronized.get_future();
    executor.attachedTextures.push_back(std::make_shared<Texture>(previous, true, [&] { synchronized.set_value(); }));
    std::promise<void> started;
    auto submit = std::async(std::launch::async, [&] { started.set_value(); executor.SubmitInternal(); });
    started.get_future().get();
    check(syncFuture.wait_for(100ms) == std::future_status::timeout,
          "staged texture synchronization waits for the previous executor submission");
    previous->NotifySubmitted();
    check(ready(syncFuture), "staged synchronization proceeds after the previous submission");
    submit.get();
    check(!previous->Poll(), "prior GPU work need not complete before recording staged synchronization");
    previous->Cancel(); current->Cancel();
}

static void bufferWaitsForPriorSubmission(bool direct) {
    vk::raii::Device oldDevice, newDevice;
    auto previous = std::make_shared<FenceCycle>(oldDevice, vk::Fence{}, vk::Semaphore{});
    auto current = std::make_shared<FenceCycle>(newDevice, vk::Fence{}, vk::Semaphore{});
    auto buffer = std::make_shared<Buffer>();
    buffer->cycle = previous;
    buffer->isDirect = direct;
    CommandExecutor executor; executor.cycle = current;
    executor.attachedBuffers.push_back(BufferView{buffer});
    std::promise<void> started;
    auto submit = std::async(std::launch::async, [&] { started.set_value(); executor.SubmitInternal(); });
    started.get_future().get();
    check(submit.wait_for(100ms) == std::future_status::timeout,
          direct ? "direct buffer waits for previous executor submission" : "clean buffer waits for previous executor submission");
    previous->NotifySubmitted();
    check(ready(submit), "buffer attachment proceeds after prior submission");
    submit.get();
    check(!previous->Poll(), "buffer dependency waits for submission without waiting for GPU completion");
    std::weak_ptr<FenceCycle> lifetime = previous;
    previous->Cancel(); previous.reset();
    check(!lifetime.expired(), "buffer predecessor lifetime remains chained after replacing its cycle");
    current->Cancel();
    check(lifetime.expired(), "buffer predecessor lifetime releases with the consumer cycle");
}

static void bufferNullAndSelfCyclesDoNotWait() {
    for (bool sameCycle : {false, true}) {
        vk::raii::Device device;
        auto current = std::make_shared<FenceCycle>(device, vk::Fence{}, vk::Semaphore{});
        Buffer buffer;
        if (sameCycle) buffer.cycle = current;
        auto update = std::async(std::launch::async, [&] { buffer.UpdateCycle(current); });
        check(ready(update), sameCycle ? "same buffer cycle does not wait for its own submission" : "null buffer predecessor does not wait");
        current->Cancel(); update.get();
        check(buffer.cycle == current, "buffer retains the new cycle");
    }
}

static bool WaitProducerExecution(FenceCycle& cycle) {
#if HAS_EXECUTION_WAIT
    return cycle.WaitForExecution();
#else
    cycle.Wait();
    return true;
#endif
}
static void executionAvailabilityPrecedesSemaphoreRetirement() {
    vk::raii::Device writerDevice, consumerDevice;
    auto writer = std::make_shared<FenceCycle>(writerDevice, vk::Fence{}, vk::Semaphore{});
    auto consumer = std::make_shared<FenceCycle>(consumerDevice, vk::Fence{}, vk::Semaphore{});
    writer->NotifySubmitted();
    consumer->NotifySubmitted();
    writer->RecordSemaphoreWaitUsage([&](vk::Semaphore) { return consumer; });
    auto object = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = object;
    writer->AttachObject(object);
    object.reset();
    writerDevice.signal();
    auto retirement = std::async(std::launch::async, [&] { writer->Wait(true); });
    consumerDevice.enteredFuture().get();
    auto data = std::async(std::launch::async, [&] { check(WaitProducerExecution(*writer), "completed producer execution is available"); });
    const bool independent = ready(data);
    const bool retained = !writer->Poll() && !lifetime.expired();
    consumerDevice.signal();
    data.get();
    retirement.get();
    check(independent, "producer data wait does not block behind semaphore consumer retirement mutex");
    check(retained, "data availability does not retire fence/semaphore/dependency lifetime");
    check(lifetime.expired(), "full retirement still releases dependencies after consumer completes");
}
#if HAS_EXECUTION_WAIT
static void canceledExecutionDoesNotPublish() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    std::promise<void> entered;
    auto start = entered.get_future();
    auto data = std::async(std::launch::async, [&] {
        entered.set_value();
        return cycle.WaitForExecution();
    });
    start.get();
    cycle.Cancel();
    check(data.wait_for(250ms) == std::future_status::ready && !data.get(), "canceled pre-submission producer is not data-ready");
    check(!cycle.PollExecution(), "cancellation does not set execution data-ready flag");
}
#endif
static void canceledPredecessorDoesNotRetireLiveFence() {
    vk::raii::Device oldDevice, currentDevice;
    auto old = std::make_shared<FenceCycle>(oldDevice, vk::Fence{}, vk::Semaphore{});
    FenceCycle current{currentDevice, {}, {}};
    current.ChainCycle(old);
    old->Cancel();
    current.NotifySubmitted();
    auto retirement = std::async(std::launch::async, [&] { current.Wait(true); });
    const bool retained = retirement.wait_for(50ms) == std::future_status::timeout;
    currentDevice.signal();
    retirement.get();
    check(retained, "canceled predecessor never skips own submitted fence retirement");
}
static void pollCannotResetFenceDuringDataWait() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    cycle.NotifySubmitted();
    std::promise<void> inDriver, release;
    auto gate = release.get_future().share();
    device.onWaitReturn = [&] {
        inDriver.set_value();
        gate.wait();
    };
    auto data = std::async(std::launch::async, [&] { return WaitProducerExecution(cycle); });
    device.signal();
    inDriver.get_future().get();
    bool reusable = cycle.Poll(false);
    if (reusable) {
        FenceCycle reset{cycle};
        reset.Cancel();
    }
    release.set_value();
    check(data.get(), "data wait observes real fence completion");
    cycle.Wait();
    check(!reusable && !device.unsafeResets, "full poll cannot publish reusable fence before raw data wait returns");
}
static void stagedWriterPublishesHostVisibility() {
    for (bool direct : {false, true}) {
        vk::raii::Device device;
        auto current = std::make_shared<FenceCycle>(device, vk::Fence{}, vk::Semaphore{});
        auto buffer = std::make_shared<Buffer>();
        buffer->isDirect = direct;
        buffer->currentExecutionGpuDirty = true;
        CommandExecutor executor;
        executor.cycle = current;
        executor.attachedBuffers.push_back({buffer});
        auto before = hostReadBarriers;
        executor.SubmitInternal();
        current->Cancel();
        check(hostReadBarriers - before == unsigned(!direct), "staged writer submission adds host visibility and direct imports retain old path");
    }
}
static void cancellationCannotResetInflightDataWait() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    cycle.NotifySubmitted();
    std::promise<void> inDriver, release;
    auto gate = release.get_future().share();
    device.onWaitReturn = [&] {
        inDriver.set_value();
        gate.wait();
    };
    auto data = std::async(std::launch::async, [&] { return WaitProducerExecution(cycle); });
    device.signal();
    inDriver.get_future().get();
    auto cancel = std::async(std::launch::async, [&] { cycle.Cancel(); });
    const bool protectedWait = cancel.wait_for(50ms) == std::future_status::timeout;
    if (!protectedWait) {
        FenceCycle reset{cycle};
        reset.Cancel();
    }
    release.set_value();
    data.get();
    cancel.get();
    check(protectedWait && !device.unsafeResets, "cancellation serializes active raw fence wait before permitting reset");
}
static void duplicateProducerDemandsShareCompletion() {
    vk::raii::Device device;
    FenceCycle cycle{device, {}, {}};
    cycle.NotifySubmitted();
    auto first = std::async(std::launch::async, [&] { return WaitProducerExecution(cycle); });
    device.enteredFuture().get();
    auto second = std::async(std::launch::async, [&] { return WaitProducerExecution(cycle); });
    device.signal();
    check(first.get() && second.get() && device.totalWaits == 1, "duplicate data demands share one raw producer wait");
    cycle.Wait();
}
int main() {
    duplicateProducerDemandsShareCompletion();
    cancellationCannotResetInflightDataWait();
    stagedWriterPublishesHostVisibility();
    canceledPredecessorDoesNotRetireLiveFence();
    pollCannotResetFenceDuringDataWait();
    executionAvailabilityPrecedesSemaphoreRetirement();
#if HAS_EXECUTION_WAIT
    canceledExecutionDoesNotPublish();
#endif
    submittedWaitIgnoresGpuCompletion();
    submissionRacingGpuWaitDoesNotTakeCompletionMutex();
    cancellationWakesSubmissionWait();
    cancellationWakesCompletionWaitBeforeSubmission();
    stagedUploadWaitsForPriorSubmission();
    bufferWaitsForPriorSubmission(false);
    bufferWaitsForPriorSubmission(true);
    bufferNullAndSelfCyclesDoNotWait();
    return failures ? 1 : 0;
}
