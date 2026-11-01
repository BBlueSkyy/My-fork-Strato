#include <common/readback_probe.h>
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
using namespace skyline::readback_probe;
int main() {
    auto before = Read(Metric::BufferCopy);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([] {
            for (int j = 0; j < 1000; ++j)
                Record(Metric::BufferCopy, 2000, 4096);
        });
    for (auto &worker : workers)
        worker.join();
    auto after = Read(Metric::BufferCopy);
    assert(after.calls - before.calls == 8000);
    assert(after.bytes - before.bytes == 8000ULL * 4096);
    assert(after.ns - before.ns == 16000000);
    assert(after.max >= 2000);
    auto nested = Read(Metric::FenceWait);
    {
        Scope outer(Metric::FenceWait);
        { Scope inner(Metric::FenceWait); }
    }
    assert(Read(Metric::FenceWait).calls - nested.calls == 1);
    {
        Scope copy(Metric::BufferCopy, 37);
        copy.End();
        copy.End();
    }
    assert(Read(Metric::BufferCopy).calls - after.calls == 1);
    assert(Read(Metric::BufferCopy).bytes - after.bytes == 37);
    auto failed = Read(Metric::TrapDispatch);
    try {
        Scope fault(Metric::TrapDispatch);
        throw 7;
    } catch (int) {
    }
    assert(Read(Metric::TrapDispatch).calls - failed.calls == 1);
    auto copies = Read(Metric::BufferCopy);
    {
        Scope outer(Metric::BufferCopy);
        { Scope inner(Metric::BufferCopy); }
    }
    assert(Read(Metric::BufferCopy).calls - copies.calls == 2); // Distinct copies are not fence recursion.
    auto domain = Read(Metric::BufferCopy);
    {
        Context buffer(Domain::Buffer, 81);
        Record(Metric::BufferCopy, 123, 7);
    }
    assert(Read(Metric::BufferCopy).domainNs[1] - domain.domainNs[1] == 123);
    std::array<std::byte, 4096> frame{};
    auto fill = [&](uint64_t esr) {
        frame.fill({});
        uint32_t magic = 0x45535201, size = 16;
        std::memcpy(frame.data(), &magic, 4);
        std::memcpy(frame.data() + 4, &size, 4);
        std::memcpy(frame.data() + 8, &esr, 8);
    };
    const auto reads = faultCounts[2].load(), unknown = faultCounts[4].load(), missing = faultCounts[7].load();
    for (uint64_t fsc : {4ULL, 7ULL, 12ULL, 15ULL}) {
        fill((0x24ULL << 26) | fsc);
        ObserveFault(SIGSEGV, SEGV_ACCERR, frame, false);
    }
    assert(faultCounts[2].load() - reads == 4);
    fill((0x24ULL << 26) | 7);
    ObserveFault(SIGSEGV, SEGV_MAPERR, frame, false);
    frame.fill({});
    ObserveFault(SIGSEGV, SEGV_ACCERR, frame, true);
    assert(faultCounts[4].load() - unknown == 2 && faultCounts[7].load() - missing == 1);
    fill((0x24ULL << 26) | 15 | 64);
    assert(fault::Classify(frame) == fault::Access::Write);
    uint32_t bad = 0xffffffff;
    std::memcpy(frame.data() + 4, &bad, 4);
    assert(fault::Classify(frame) == fault::Access::Unknown);
    const auto overflow = detail::threadOverflow.load();
    for (int i = 0; i < 80; ++i) {
        std::thread t([] { Scope trap(Metric::TrapDispatch); });
        t.join();
    }
    assert(detail::threadOverflow.load() > overflow);
    const auto sampleBefore = detail::sampleSequence.load();
    {
        Context resource(Domain::Buffer, 123);
        CycleContext cycle(456);
        Scope wait(Metric::ProducerWait);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool sampled{};
    for (auto &slot : detail::samples) {
        if (slot.busy.test_and_set(std::memory_order_acquire))
            continue;
        const auto sample = slot.sample;
        slot.busy.clear(std::memory_order_release);
        sampled |= sample.sequence > sampleBefore && sample.resource == 123 && sample.cycle == 456 &&
                   sample.domain == Domain::Buffer;
    }
    assert(sampled);
    std::cout << "PASS concurrent exact counters, nested fence deduplication, end-once, exception scopes\n";
}
