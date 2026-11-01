#include <common/readback_probe.h>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <iostream>
#define TRACE_EVENT(...)
namespace skyline::gpu {
    struct Cycle {
        unsigned waits{};
        bool fail{};
        void Wait() {
            ++waits;
            if (fail)
                throw std::runtime_error("fence failure");
        }
    };
    struct Texture {
        std::shared_ptr<Cycle> cycle;
        void WaitOnFence();
    };
#include "texture_wait.inc"
} // namespace skyline::gpu
int main() {
    using namespace skyline;
    auto before = readback_probe::Read(readback_probe::Metric::ProducerWait);
    gpu::Texture texture;
    texture.cycle = std::make_shared<gpu::Cycle>();
    auto cycle = texture.cycle;
    texture.WaitOnFence();
    assert(cycle->waits == 1 && !texture.cycle);
    assert(readback_probe::Read(readback_probe::Metric::ProducerWait).calls - before.calls == 1);
    texture.WaitOnFence();
    assert(readback_probe::Read(readback_probe::Metric::ProducerWait).calls - before.calls == 1);
    texture.cycle = cycle;
    cycle->fail = true;
    bool failed{};
    try {
        texture.WaitOnFence();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed && texture.cycle == cycle);
    cycle->fail = false;
    texture.WaitOnFence();
    assert(!texture.cycle && cycle->waits == 3);
    std::cout << "PASS actual texture producer wait counted; absent cycle skipped; failure retains dependency and "
                 "retry recovers\n";
}
