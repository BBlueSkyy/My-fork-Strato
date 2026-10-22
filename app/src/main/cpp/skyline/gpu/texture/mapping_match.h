// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace skyline::gpu::texture {
    struct MappingSubrange {
        size_t offset; //!< Logical byte offset, independent of physical segment order.
        bool full;
    };

    // Views may clip the first and last physical span, but every interior
    // boundary must match. Equal end addresses alone permit remapped segments
    // to reuse stale images. Keep segmentation strict, as alias registration
    // and guest traps currently require identical ordered spans.
    template<typename HostMappings, typename HostIterator, typename GuestMappings>
    std::optional<MappingSubrange> MatchMappingSubrange(const HostMappings &hostMappings, HostIterator firstHost,
                                                       const GuestMappings &guestMappings) {
        if (guestMappings.empty() || firstHost == hostMappings.end())
            return std::nullopt;

        auto host{firstHost};
        for (auto guest{guestMappings.begin()}; guest != guestMappings.end(); ++guest, ++host) {
            if (host == hostMappings.end() || guest->empty() || host->empty())
                return std::nullopt;
            const auto guestBegin{reinterpret_cast<uintptr_t>(guest->data())};
            const auto hostBegin{reinterpret_cast<uintptr_t>(host->data())};
            if (!guestBegin || !hostBegin || guestBegin < hostBegin)
                return std::nullopt;
            const auto offset{guestBegin - hostBegin};
            if ((guest != guestMappings.begin() && offset != 0) || offset >= host->size() ||
                guest->size() > host->size() - offset)
                return std::nullopt;
            if (guest + 1 != guestMappings.end() && guest->size() != host->size() - offset)
                return std::nullopt;
        }

        size_t offset{reinterpret_cast<uintptr_t>(guestMappings.front().data()) - reinterpret_cast<uintptr_t>(firstHost->data())};
        for (auto it{hostMappings.begin()}; it != firstHost; ++it)
            offset += it->size();
        const bool full{firstHost == hostMappings.begin() && guestMappings.front().data() == firstHost->data() &&
                       host == hostMappings.end() && guestMappings.back().size() == (host - 1)->size()};
        return MappingSubrange{offset, full};
    }
}
