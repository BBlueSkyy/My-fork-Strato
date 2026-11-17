// SPDX-License-Identifier: MIT OR MPL-2.0
#include <services/nvdrv/devices/nvhost/as_gpu_mapping_lifecycle.h>

#include <cassert>
#include <cstdint>
#include <list>
#include <map>
#include <memory>

using skyline::service::nvdrv::device::nvhost::detail::ForgetFixedMapping;
using skyline::service::nvdrv::device::nvhost::detail::FitsMappingSubrange;

struct Mapping {
    std::uint64_t offset;
    bool fixed;
};
struct Allocation {
    std::uint64_t size;
    std::list<std::shared_ptr<Mapping>> mappings;
};
using Allocations = std::map<std::uint64_t, Allocation>;

int main() {
    assert(FitsMappingSubrange(0x4000, 0x1000, 0x3000));
    assert(FitsMappingSubrange(0x4000, 0, 0x4000));
    assert(!FitsMappingSubrange(0x4000, 0x1000, 0x3001));
    assert(!FitsMappingSubrange(0x4000, 0x4001, 0));
    assert(!FitsMappingSubrange(0x4000, UINT64_MAX, 0x4000));
    assert(!FitsMappingSubrange(0x4000, 0x1000, UINT64_MAX));

    constexpr std::uint64_t base{0x500000000ULL};
    Allocations allocations;
    allocations.emplace(base, Allocation{0x80000, {}});
    auto fixedA{std::make_shared<Mapping>(Mapping{base + 0x10000, true})};
    auto fixedB{std::make_shared<Mapping>(Mapping{base + 0x30000, true})};
    allocations.at(base).mappings = {fixedA, fixedB};

    // UnmapBuffer before FreeSpace must not leave a stale owner reference.
    ForgetFixedMapping(allocations, fixedA);
    assert(allocations.at(base).mappings.size() == 1);
    assert(allocations.at(base).mappings.front() == fixedB);

    // FreeSpace copies the remaining list before FreeMappingLocked removes
    // from the owning list. Removing during this traversal is safe.
    const auto remaining{allocations.at(base).mappings};
    for (const auto &mapping : remaining)
        ForgetFixedMapping(allocations, mapping);
    assert(allocations.at(base).mappings.empty());

    // Repeated removal must not affect another fixed mapping with same address.
    auto replacement{std::make_shared<Mapping>(Mapping{fixedA->offset, true})};
    allocations.at(base).mappings.push_back(replacement);
    ForgetFixedMapping(allocations, fixedA);
    assert(allocations.at(base).mappings.front() == replacement);

    // Non-fixed mappings do not belong to an AllocSpace record.
    auto dynamic{std::make_shared<Mapping>(Mapping{base + 0x20000, false})};
    ForgetFixedMapping(allocations, dynamic);
    assert(allocations.at(base).mappings.front() == replacement);

    // Fixed address outside all reservations cannot touch an unrelated owner.
    auto outside{std::make_shared<Mapping>(Mapping{base + 0x90000, true})};
    auto before{std::make_shared<Mapping>(Mapping{base - 0x1000, true})};
    ForgetFixedMapping(allocations, outside);
    ForgetFixedMapping(allocations, before);
    assert(allocations.at(base).mappings.front() == replacement);

    ForgetFixedMapping(allocations, replacement);
    assert(allocations.at(base).mappings.empty());
}
