// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>
#include "../../app/src/main/cpp/skyline/gpu/texture/mapping_match.h"

int main() {
    using Mapping = std::span<std::uint8_t>;
    using skyline::gpu::texture::MatchMappingSubrange;
    std::array<std::uint8_t, 1024> memory{};
    // Physical order deliberately differs from the logical texture order.
    std::vector<Mapping> host{{memory.data() + 400, 32}, {memory.data() + 128, 32}, {memory.data() + 700, 32}};
    auto guest{host};
    auto match{MatchMappingSubrange(host, host.begin(), guest)};
    assert(match && match->offset == 0 && match->full);
    // Equal ends do not imply equal physical ranges after remapping.
    guest[1] = {memory.data() + 144, 16};
    assert(!MatchMappingSubrange(host, host.begin(), guest));
    guest = host;
    guest[1] = {memory.data() + 160, 32};
    assert(!MatchMappingSubrange(host, host.begin(), guest));
    guest = host;
    guest.pop_back();
    guest[0] = {memory.data() + 408, 24};
    guest[1] = {memory.data() + 128, 12};
    match = MatchMappingSubrange(host, host.begin(), guest);
    assert(match && match->offset == 8 && !match->full);
    // A mip/layer starting in a secondary segment uses a logical byte offset.
    guest = {{memory.data() + 136, 24}, {memory.data() + 700, 8}};
    match = MatchMappingSubrange(host, host.begin() + 1, guest);
    assert(match && match->offset == 40 && !match->full);
    guest = {{memory.data() + 136, 24}, {memory.data() + 704, 8}};
    assert(!MatchMappingSubrange(host, host.begin() + 1, guest));
    guest = {{memory.data() + 136, 25}};
    assert(!MatchMappingSubrange(host, host.begin() + 1, guest));
    guest = {{memory.data() + 160, 1}};
    assert(!MatchMappingSubrange(host, host.begin() + 1, guest));
    guest = {{memory.data() + 400, 32}};
    match = MatchMappingSubrange(host, host.begin(), guest);
    assert(match && match->offset == 0 && !match->full);
    guest = {{memory.data() + 400, 32}, {}};
    assert(!MatchMappingSubrange(host, host.begin(), guest));
    guest.clear();
    assert(!MatchMappingSubrange(host, host.begin(), guest));
    assert(!MatchMappingSubrange(host, host.end(), host));
    std::vector<Mapping> single{{memory.data() + 400, 32}};
    match = MatchMappingSubrange(single, single.begin(), single);
    assert(match && match->full && match->offset == 0);
}
