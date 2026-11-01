#include <gpu/fence_cycle.h>
#include <future>
#include <chrono>
#include <iostream>
#include "trace_clock.h"
using namespace std::chrono_literals;
using skyline::gpu::FenceCycle;
namespace bp = skyline::gpu::buffer_readback_probe;
static void Require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
static void Blocked(std::future<void> &f) {
    Require(f.wait_for(25ms) == std::future_status::timeout, "wait was incorrectly bypassed");
}
static void Dominant(const bp::WaitTrace &trace, bp::WaitPart part) {
    Require(trace.parts[part] >= 15000000, "wrong wait phase attribution");
    uint64_t sum{};
    for (auto ns : trace.parts)
        sum += ns;
    Require(sum <= trace.Elapsed(), "root wait phases overlap");
    Require(trace.depth == 0, "recursive trace depth leaked");
}
int main() {
    {
        vk::raii::Device d;
        FenceCycle c{d, {}, {}};
        d.signal();
        bp::WaitTrace trace{1, 4096, true};
        std::promise<void> phaseEntered;
        auto f = std::async(std::launch::async, [&] { trace_test::Wait(c, trace, phaseEntered, 2); });
        phaseEntered.get_future().get();
        Blocked(f);
        c.NotifySubmitted();
        f.get();
        Dominant(trace, bp::Submit);
    }
    {
        vk::raii::Device d;
        FenceCycle c{d, {}, {}};
        c.NotifySubmitted();
        bp::WaitTrace trace{2, 4096, true};
        auto entered = d.enteredFuture();
        auto f = std::async(std::launch::async, [&] { c.Wait(false, &trace); });
        entered.get();
        Blocked(f);
        d.signal();
        f.get();
        Dominant(trace, bp::Driver);
    }
    {
        vk::raii::Device d;
        FenceCycle c{d, {}, {}};
        c.NotifySubmitted();
        auto entered = d.enteredFuture();
        auto background = std::async(std::launch::async, [&] { c.Wait(); });
        entered.get();
        bp::WaitTrace trace{3, 4096, true};
        std::promise<void> phaseEntered;
        auto f = std::async(std::launch::async, [&] { trace_test::Wait(c, trace, phaseEntered, 3); });
        phaseEntered.get_future().get();
        Blocked(f);
        d.signal();
        f.get();
        background.get();
        Dominant(trace, bp::Mutex);
    }
    {
        vk::raii::Device oldDevice, newDevice;
        auto old = std::make_shared<FenceCycle>(oldDevice, vk::Fence{}, vk::Semaphore{});
        FenceCycle current{newDevice, {}, {}};
        old->NotifySubmitted();
        current.NotifySubmitted();
        newDevice.signal();
        current.ChainCycle(old);
        auto entered = oldDevice.enteredFuture();
        bp::WaitTrace trace{4, 4096, true};
        auto f = std::async(std::launch::async, [&] { current.Wait(false, &trace); });
        entered.get();
        Blocked(f);
        oldDevice.signal();
        f.get();
        Dominant(trace, bp::Chain);
        Require(trace.nodes == 2, "nested cycle not counted");
    }
    {
        vk::raii::Device producerDevice, consumerDevice;
        auto consumer = std::make_shared<FenceCycle>(consumerDevice, vk::Fence{}, vk::Semaphore{});
        consumer->NotifySubmitted();
        FenceCycle producer{producerDevice, {}, {}};
        producer.NotifySubmitted();
        producerDevice.signal();
        producer.RecordSemaphoreWaitUsage([&](auto) { return consumer; });
        auto entered = consumerDevice.enteredFuture();
        bp::WaitTrace trace{5, 4096, true};
        auto f = std::async(std::launch::async, [&] { producer.Wait(false, &trace); });
        entered.get();
        Blocked(f);
        consumerDevice.signal();
        f.get();
        Dominant(trace, bp::Tail);
        Require(trace.nodes == 2, "semaphore consumer not counted");
    }
    {
        vk::raii::Device d;
        FenceCycle c{d, {}, {}};
        bp::WaitTrace trace{6, 4096, true};
        std::promise<void> phaseEntered;
        auto f = std::async(std::launch::async, [&] { trace_test::Wait(c, trace, phaseEntered, 2); });
        phaseEntered.get_future().get();
        Blocked(f);
        c.Cancel();
        f.get();
        Dominant(trace, bp::Submit);
    }
    std::cout << "PASS production FenceCycle sampled submission, driver, mutex, chain, semaphore tail, cancellation; "
                 "disjoint root phases\n";
}
