// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <iostream>
#include <stdexcept>
#include <gpu/guest_buffer.h>

using namespace skyline;
using namespace skyline::gpu;

static void Check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

int main() {
    std::array<u8, 0x4000> a{};
    std::array<u8, 0x4000> b{};
    std::array<u8, 0x4000> c{};

    GuestBuffer single{span<u8>{a.data(), a.size()}};
    Check(single.Find(span<u8>{a.data() + 0x100, 0x200}) == 0x100, "single mapping offset failed");

    GuestBuffer split{GuestBuffer::Mappings{
        span<u8>{a.data(), 0x1000},
        span<u8>{b.data(), 0x1000},
        span<u8>{c.data(), 0x1000},
    }};
    Check(split.size() == 0x3000, "split size failed");

    GuestBuffer middle{GuestBuffer::Mappings{
        span<u8>{b.data() + 0x100, 0xF00},
        span<u8>{c.data(), 0x100},
    }};
    Check(split.Find(middle) == 0x1100, "split logical subrange failed");

    GuestBuffer reordered{GuestBuffer::Mappings{
        span<u8>{c.data(), 0x100},
        span<u8>{b.data(), 0x100},
    }};
    Check(!split.Find(reordered), "reordered alias was accepted as affine");

    GuestBuffer repeatedAlias{GuestBuffer::Mappings{
        span<u8>{a.data(), 0x1000},
        span<u8>{b.data(), 0x1000},
        span<u8>{a.data(), 0x1000},
    }};
    Check(!repeatedAlias.valid(), "repeated physical alias was accepted");

    GuestBuffer overlappingAlias{GuestBuffer::Mappings{
        span<u8>{a.data(), 0x1000},
        span<u8>{a.data() + 0x800, 0x1000},
    }};
    Check(!overlappingAlias.valid(), "overlapping physical alias was accepted");

    GuestBuffer invalid{GuestBuffer::Mappings{
        span<u8>{a.data(), 0x1000},
        span<u8>{nullptr, 0x1000},
    }};
    Check(!invalid.valid(), "unmapped split region was accepted");

    GuestBuffer adjacent{GuestBuffer::Mappings{
        span<u8>{a.data(), 0x1000},
        span<u8>{a.data() + 0x1000, 0x1000},
    }};
    Check(adjacent.mappings.size() == 1 && adjacent.size() == 0x2000, "adjacent mappings were not normalized");

    std::cout << "split buffer mapping tests: PASS\n";
}
