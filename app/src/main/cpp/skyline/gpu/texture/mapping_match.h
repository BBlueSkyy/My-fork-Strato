// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace skyline::gpu::texture {
    /**
     * Finds the offset in a logical sequence of host mappings occupied by a
     * requested physical memory sequence. The segmentation of host/request
     * mappings may differ, but every byte must be the same physical byte in
     * the same logical order. No pointer subtraction across allocations.
     *
     * firstHostIndex is the mapping already known to contain the request's
     * first byte. Matching the first byte alone is not sufficient.
     */
    template<typename HostMappings, typename GuestMappings>
    std::optional<std::size_t> FindContainedMappingOffset(const HostMappings &host,
                                                            const GuestMappings &guest,
                                                            std::size_t firstHostIndex) {
        if (host.empty() || guest.empty() || firstHostIndex >= host.size() ||
            !guest.front().size() || !host[firstHostIndex].size())
            return std::nullopt;

        const auto hostStart{reinterpret_cast<std::uintptr_t>(host[firstHostIndex].data())};
        const auto guestStart{reinterpret_cast<std::uintptr_t>(guest.front().data())};
        if (guestStart < hostStart || guestStart - hostStart >= host[firstHostIndex].size())
            return std::nullopt;

        std::size_t logicalOffset{};
        for (std::size_t i{}; i < firstHostIndex; ++i) {
            if (host[i].size() > std::numeric_limits<std::size_t>::max() - logicalOffset)
                return std::nullopt;
            logicalOffset += host[i].size();
        }
        const auto firstOffset{static_cast<std::size_t>(guestStart - hostStart)};
        if (firstOffset > std::numeric_limits<std::size_t>::max() - logicalOffset)
            return std::nullopt;
        logicalOffset += firstOffset;

        std::size_t hi{firstHostIndex}, ho{firstOffset};
        for (const auto &segment : guest) {
            if (!segment.size())
                return std::nullopt;
            std::size_t gi{};
            while (gi < segment.size()) {
                while (hi < host.size() && ho == host[hi].size()) {
                    ++hi;
                    ho = 0;
                }
                if (hi == host.size())
                    return std::nullopt;
                const auto n{std::min(host[hi].size() - ho, segment.size() - gi)};
                const auto hp{reinterpret_cast<std::uintptr_t>(host[hi].data())};
                const auto gp{reinterpret_cast<std::uintptr_t>(segment.data())};
                if (!n || hp > std::numeric_limits<std::uintptr_t>::max() - ho ||
                    gp > std::numeric_limits<std::uintptr_t>::max() - gi ||
                    hp + ho != gp + gi)
                    return std::nullopt;
                gi += n;
                ho += n;
            }
        }
        return logicalOffset;
    }
}
