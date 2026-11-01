#pragma once
#include <condition_variable>
#include <atomic>
#include <functional>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
using VkFence = void *;
namespace vk {
enum class Result { eSuccess, eTimeout, eErrorInitializationFailed, eNotReady };
inline std::string to_string(Result) { return "fake Vulkan result"; }
struct Fence { operator VkFence() const { return nullptr; } };
struct Semaphore {};
namespace raii {
using CommandBuffer = int;
// Vulkan facade: waitForFences is held at a gate after production Wait owns its
// completion mutex. Only signal() opens that gate; CPU submission must not.
class Device {
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable bool signalled{};
    mutable std::promise<void> entered;
    mutable std::once_flag enteredOnce;
public:
    mutable std::atomic<unsigned> rawWaits{}, unsafeResets{}, totalWaits{};
    std::function<void()> onWaitReturn;
    std::future<void> enteredFuture() const { return entered.get_future(); }
    void signal() const { { std::scoped_lock lock{mutex}; signalled = true; } condition.notify_all(); }
    void resetFences(Fence) const { if(rawWaits)++unsafeResets; std::scoped_lock lock{mutex}; signalled = false; }
    const Device &operator*() const { return *this; }
    const Device *getDispatcher() const { return this; }
    Result waitForFences(unsigned, const Fence *, bool, std::uint64_t, const Device &) const {
        ++rawWaits;++totalWaits;
        std::call_once(enteredOnce, [&] { entered.set_value(); });
        std::unique_lock lock{mutex}; condition.wait(lock, [&] { return signalled; });
        lock.unlock();if(onWaitReturn)onWaitReturn();--rawWaits;
        return Result::eSuccess;
    }
    Result getFenceStatus(Fence, const Device &) const {
        std::scoped_lock lock{mutex}; return signalled ? Result::eSuccess : Result::eNotReady;
    }
};
}
}
