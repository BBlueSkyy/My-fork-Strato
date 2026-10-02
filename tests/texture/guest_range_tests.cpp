#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <skyline/gpu/texture/guest_range.h>
#include <skyline/gpu/texture/mapping_cache.h>

namespace skyline::gpu::texture {
    class TextureStorage {};
}

using namespace skyline::gpu::texture;

int main() {
    std::array<std::uint8_t, 4096> memory{};
    const std::array<std::span<std::uint8_t>, 2> backing{
        std::span{memory}.subspan(100, 64),
        std::span{memory}.subspan(1000, 128),
    };
    const GuestResourceRanges parent(backing);
    assert(parent.Size() == 192);
    assert(parent.FindUniqueOffset(memory.data() + 100) == 0);
    assert(parent.FindUniqueOffset(memory.data() + 1005) == 69);
    assert(!parent.FindUniqueOffset(memory.data() + 500));

    const std::array<std::span<std::uint8_t>, 3> middle{
        std::span{memory}.subspan(140, 24),
        std::span{memory}.subspan(1000, 20),
        std::span{memory}.subspan(1020, 20),
    };
    const GuestResourceRanges child(middle);
    assert(parent.FindContainedOffset(child) == 40);
    assert(parent.Overlaps(child));

    const auto slices{parent.Slice(60, 10)};
    assert(slices.size() == 2);
    assert(slices[0].address == reinterpret_cast<std::uintptr_t>(memory.data() + 160));
    assert(slices[0].size == 4 && slices[0].logicalOffset == 60);
    assert(slices[1].address == reinterpret_cast<std::uintptr_t>(memory.data() + 1000));
    assert(slices[1].size == 6 && slices[1].logicalOffset == 64);

    const std::array<std::span<std::uint8_t>, 1> single{
        std::span{memory}.subspan(2000, 192),
    };
    const GuestResourceRanges one(single);
    assert(one.Size() == 192);
    assert(!parent.Overlaps(one));
    assert(!parent.FindContainedOffset(one));

    const std::array<std::span<std::uint8_t>, 2> contiguous{
        std::span{memory}.subspan(2000, 80),
        std::span{memory}.subspan(2080, 112),
    };
    assert(one.FindContainedOffset(GuestResourceRanges(contiguous)) == 0);

    const std::array<std::span<std::uint8_t>, 2> badTail{
        std::span{memory}.subspan(140, 24),
        std::span{memory}.subspan(1200, 40),
    };
    assert(parent.Overlaps(GuestResourceRanges(badTail)));
    assert(!parent.FindContainedOffset(GuestResourceRanges(badTail)));
    assert(!IsCompleteGuestAlias(parent, GuestResourceRanges(badTail)));
    assert(IsCompleteGuestAlias(parent, child));
    assert(!IsCompleteGuestAlias(parent, one));
    const std::array<std::span<std::uint8_t>, 2> secondSpanOnly{
        std::span{memory}.subspan(2000, 16),
        std::span{memory}.subspan(1000, 16),
    };
    assert(parent.Overlaps(GuestResourceRanges(secondSpanOnly)));
    assert(!IsCompleteGuestAlias(parent, GuestResourceRanges(secondSpanOnly)));

    const std::array<std::span<std::uint8_t>, 2> repeated{
        std::span{memory}.subspan(100, 32),
        std::span{memory}.subspan(100, 32),
    };
    assert(!GuestResourceRanges(repeated).FindUniqueOffset(memory.data() + 100));

    const std::array<std::size_t, 2> mipSizes{64, 32};
    auto mip{LocateSubresource(128 + 64, 128, mipSizes, 2)};
    assert(mip && mip->layer == 1 && mip->mip == 1 && mip->offsetWithinMip == 0);
    assert(!LocateSubresource(100, 128, mipSizes, 2)); // Layer padding is not a mip.
    const std::array<std::size_t, 2> invalidMipSizes{100, 100};
    assert(!LocateSubresource(120, 128, invalidMipSizes, 2));

    TextureMappingCache cache;
    auto storage{std::make_shared<TextureStorage>()};
    auto unrelated{std::make_shared<TextureStorage>()};
    cache.Insert(storage, parent);
    cache.Insert(unrelated, one);
    auto lookup{cache.Lookup(child)};
    assert(lookup.storages.size() == 1 && lookup.storages[0] == storage);

    const std::array<std::span<std::uint8_t>, 1> onlySecond{
        std::span{memory}.subspan(1008, 16),
    };
    lookup = cache.Lookup(GuestResourceRanges(onlySecond));
    assert(lookup.storages.size() == 1 && lookup.storages[0] == storage);

    const std::array<std::span<std::uint8_t>, 1> nested{
        std::span{memory}.subspan(120, 8),
    };
    cache.Insert(std::make_shared<TextureStorage>(), GuestResourceRanges(nested));
    lookup = cache.Lookup(GuestResourceRanges(nested));
    assert(lookup.storages.size() == 2); // Nested ranges must not hide the parent.
    assert(lookup.firstMappingOverlaps.size() == 2);
}
