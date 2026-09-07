// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <future>
#include <common/spin_lock.h>

// Exercise the production fallback deterministically on a host without ARM instructions.
namespace skyline {
    void AdaptiveSingleWaiterConditionVariable::SpinWait() {}
    void AdaptiveSingleWaiterConditionVariable::SpinWait(std::chrono::steady_clock::time_point) {}
}

int main() {
    using namespace std::chrono_literals;
    skyline::AdaptiveSingleWaiterConditionVariable condition;
    std::mutex mutex;
    unsigned generation{};
    for (unsigned i = 1; i <= 1000; ++i) {
        std::promise<void> entering;
        auto entered = entering.get_future();
        auto waiter = std::async(std::launch::async, [&] {
            std::unique_lock lock{mutex};
            entering.set_value();
            condition.wait(lock, [&] { return generation == i; });
            assert(lock.owns_lock());
        });
        entered.get();
        {
            std::scoped_lock lock{mutex};
            generation = i;
            condition.notify();
        }
        assert(waiter.wait_for(2s) == std::future_status::ready);
        waiter.get();
    }
    std::unique_lock lock{mutex};
    assert(!condition.wait_for(lock, 2ms, [] { return false; }));
    assert(condition.wait_for(lock, std::chrono::nanoseconds::max(), [] { return true; }));
    assert(lock.owns_lock());
}
