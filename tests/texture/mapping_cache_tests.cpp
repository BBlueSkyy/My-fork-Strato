// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>
#include <skyline/gpu/texture/mapping_cache.h>

using namespace skyline::gpu::texture;

int main() {
    std::vector<std::uint8_t> memory(262144);
    const std::span bytes{memory};
    auto make = [](const auto &mappings) {
        return std::make_shared<TextureStorage>(nullptr, GuestResourceRanges{mappings});
    };
    const std::array parentMaps{bytes.subspan(100,64), bytes.subspan(1000,128)};
    auto parent{make(parentMaps)};
    TextureMappingCache cache;
    cache.Insert(parent);
    const std::array childMaps{bytes.subspan(140,24),bytes.subspan(1000,40)};
    auto lookup{cache.Lookup(GuestResourceRanges{childMaps})};
    assert(lookup.storages.size()==1 && lookup.storages[0]==parent);
    const std::array secondOnly{bytes.subspan(1008,16)};
    assert(cache.Lookup(GuestResourceRanges{secondOnly}).storages==lookup.storages);
    const std::array nestedMap{bytes.subspan(120,8)};
    auto nested{make(nestedMap)};
    cache.Insert(nested);
    lookup=cache.Lookup(GuestResourceRanges{nestedMap});
    assert(lookup.storages.size()==2 && lookup.firstMappingOverlaps.size()==2);
    assert(lookup.firstMappingOverlaps[0]->storage==nested);
    // Equal-address entries preserve insertion precedence in reverse candidate order.
    auto same{make(nestedMap)};
    cache.Insert(same);
    lookup=cache.Lookup(GuestResourceRanges{nestedMap});
    assert(lookup.firstMappingOverlaps[0]->storage==same);
    const std::array endpoint{bytes.subspan(164,8)};
    assert(cache.Lookup(GuestResourceRanges{endpoint}).storages.empty());
    const std::array<std::span<std::uint8_t>,0> empty{};
    assert(cache.Lookup(GuestResourceRanges{empty}).storages.empty());
    cache.Insert(nullptr);

    TextureMappingCache sliceCache;
    const std::array split{bytes.subspan(2000,16),bytes.subspan(100,16)};
    auto splitStorage{make(split)};
    sliceCache.Insert(splitStorage);
    sliceCache.Insert(parent);
    const auto ordered{sliceCache.StoragesInMappingOrder()};
    assert(ordered.size()==2 && ordered[0]==splitStorage && ordered[1]==parent);
    const std::array laterQuery{bytes.subspan(3000,16), bytes.subspan(1008,16)};
    lookup=sliceCache.Lookup(GuestResourceRanges{laterQuery});
    assert(lookup.firstMappingOverlaps.empty() && lookup.storages.size()==1 && lookup.storages[0]==parent);

    // Out-of-order inserts, fragmentation, nested ranges and duplicate addresses
    // are compared with a full physical-overlap oracle, not the index algorithm.
    TextureMappingCache randomCache;
    std::vector<std::shared_ptr<TextureStorage>> storages;
    std::mt19937 random{244};
    for (unsigned insert{}; insert<300; ++insert) {
        const auto first{random()%250000}, second{random()%250000};
        const std::array maps{bytes.subspan(first,1+random()%4096),bytes.subspan(second,1+random()%4096)};
        auto storage{make(maps)};
        randomCache.Insert(storage);
        storages.push_back(storage);
    }
    for (unsigned query{}; query<3000; ++query) {
        const std::array maps{bytes.subspan(random()%250000,1+random()%4096),bytes.subspan(random()%250000,1+random()%4096)};
        const GuestResourceRanges ranges{maps};
        const auto result{randomCache.Lookup(ranges)};
        size_t expected{};
        size_t firstCount{};
        for (const auto &storage : storages) {
            const auto overlaps{storage->ranges.Overlaps(ranges)};
            const auto count{std::count(result.storages.begin(),result.storages.end(),storage)};
            assert(count==(overlaps ? 1 : 0));
            expected+=overlaps;
            const auto &first{ranges.Segments().front()};
            for (const auto &segment : storage->ranges.Segments())
                firstCount+=segment.address<first.End() && first.address<segment.End();
        }
        assert(result.storages.size()==expected && result.firstMappingOverlaps.size()==firstCount);
        for (size_t i{1}; i<result.firstMappingOverlaps.size(); ++i)
            assert(result.firstMappingOverlaps[i-1]->address>=result.firstMappingOverlaps[i]->address);
    }

    std::weak_ptr<TextureStorage> lifetime;
    {
        TextureMappingCache owningCache;
        auto storage{make(parentMaps)};
        lifetime=storage;
        owningCache.Insert(storage);
        storage.reset();
        assert(!lifetime.expired());
    }
    assert(lifetime.expired());
}
