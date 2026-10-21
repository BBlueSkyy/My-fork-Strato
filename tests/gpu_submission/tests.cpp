#include <gpu/fence_cycle.h>
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
    bool guest{true}, isDirect{};
    enum class DirtyState { CpuDirty, Clean, GpuDirty } dirtyState{DirtyState::Clean};
    std::mutex stateMutex;
    Texture::Gpu gpu;
    std::shared_ptr<int> trapHandle{std::make_shared<int>(0)};
    std::shared_ptr<std::vector<int>> backing{std::make_shared<std::vector<int>>()};
    std::vector<int> mirror;
    bool RequiresCycleAttach() const { return true; }
    void SynchronizeHost(bool skipTrap = false);
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
struct CommandExecutor {
    bool renderPass{};
    Slot storage;
    Slot *slot{&storage};
    std::shared_ptr<FenceCycle> cycle;
    std::vector<std::shared_ptr<Texture>> attachedTextures, preserveAttachedTextures;
    std::vector<BufferView> attachedBuffers, preserveAttachedBuffers;
    std::vector<std::function<void()>> diagnosticUploadCallbacks;
    void FinishRenderPass() {}
    void RotateRecordSlot() {}
    void SubmitInternal();
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
    bool proofPrepared{};
    executor.diagnosticUploadCallbacks.emplace_back([&] {
        check(syncFuture.wait_for(0ms) == std::future_status::ready,
              "upload provenance is prepared after actual guest-to-host synchronization");
        proofPrepared = true;
    });
    std::promise<void> started;
    auto submit = std::async(std::launch::async, [&] { started.set_value(); executor.SubmitInternal(); });
    started.get_future().get();
    check(syncFuture.wait_for(100ms) == std::future_status::timeout,
          "staged texture synchronization waits for the previous executor submission");
    previous->NotifySubmitted();
    check(ready(syncFuture), "staged synchronization proceeds after the previous submission");
    submit.get();
    check(proofPrepared && executor.diagnosticUploadCallbacks.empty(),
          "provenance preparation runs once before the slot is handed to the recording thread");
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

int main() {
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
