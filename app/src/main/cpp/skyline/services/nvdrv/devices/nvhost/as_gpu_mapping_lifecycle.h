// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2026 Skyline Team and Contributors

#pragma once

#include <iterator>
#include <cstdint>

namespace skyline::service::nvdrv::device::nvhost::detail {
    /** Validate the region to remap without overflowing offset + length. */
    constexpr bool FitsMappingSubrange(std::uint64_t totalSize, std::uint64_t offset, std::uint64_t length) {
        return offset <= totalSize && length <= totalSize - offset;
    }

    /**
     * @brief Forget a fixed GPU mapping when it is explicitly unmapped.
     *
     * Fixed mappings also live in their parent AllocSpace record. Leaving a
     * stale shared_ptr there causes FreeSpace to try to unmap it a second time,
     * and to abort without releasing the reservation when mappingMap.at fails.
     *
     * This does not change the GMMU mapping or the allocator. The caller must
     * hold the AsGpu mutex, and must remove the mapping from mappingMap itself.
     */
    template <typename AllocationMap, typename MappingPtr>
    void ForgetFixedMapping(AllocationMap &allocations, const MappingPtr &mapping) {
        if (!mapping->fixed)
            return;

        auto upper{allocations.upper_bound(mapping->offset)};
        if (upper == allocations.begin())
            return;

        auto allocation{std::prev(upper)};
        // Check that the mapping actually belongs to this AllocSpace record.
        if (mapping->offset - allocation->first >= allocation->second.size)
            return;

        allocation->second.mappings.remove(mapping);
    }
}
