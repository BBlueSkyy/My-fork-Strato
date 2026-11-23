// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include <soc/host1x/syncpoint.h>

using namespace skyline::soc::host1x;
int main() {
    Syncpoint point;
    std::vector<unsigned> completed;
    point.RegisterWaiter(3, [&] { completed.push_back(3); });
    point.RegisterWaiter(1, [&] { completed.push_back(1); });
    auto cancelled = point.RegisterWaiter(2, [&] { assert(false); });
    point.DeregisterWaiter(cancelled);
    assert(point.Increment() == 1);
    assert((completed == std::vector<unsigned>{1}));
    assert(point.Increment() == 2);
    assert(completed.size() == 1);
    assert(point.Increment() == 3);
    assert((completed == std::vector<unsigned>{1, 3}));
    point.RegisterWaiter(2, [&] { completed.push_back(2); });
    assert(completed.back() == 2); // Already reached: immediate callback.

    SyncpointPair pair;
    pair.guest.Increment(); // Recording advances guest, not host completion.
    assert(pair.guest.Load() == 1 && pair.host.Load() == 0);
    assert(!pair.host.Wait(1, std::chrono::steady_clock::duration::zero()));
    std::promise<void> entered;
    auto ready = entered.get_future();
    std::thread consumer([&] {
        entered.set_value();
        // Test the completion condition, independently of fast/slow enrollment.
        (void)pair.host.Wait(1, std::chrono::steady_clock::duration::max());
        assert(pair.host.Load() >= 1);
    });
    ready.get();
    pair.host.Increment();
    consumer.join();
    pair.Increment();
    assert(pair.guest.Load() == 2 && pair.host.Load() == 2);
    std::cout << "host1x threshold order, deregistration, immediate callbacks and guest/host completion: PASS\n";
}
