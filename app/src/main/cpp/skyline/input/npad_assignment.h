// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>

namespace skyline::input {
    template <std::size_t PadCount, std::size_t ControllerCount>
    class NpadAssignmentOrder {
        std::array<int, PadCount> preferred{};
        std::array<bool, PadCount> remembered{};

      public:
        NpadAssignmentOrder() { Reset(); }

        void Remember(std::size_t pad, int source) {
            // A controller source can belong to only one persistent Npad assignment.
            if (source >= 0 && static_cast<std::size_t>(source) < ControllerCount) {
                for (std::size_t other{}; other < PadCount; ++other) {
                    if (other != pad && remembered[other] && preferred[other] == source) {
                        preferred[other] = -1;
                        remembered[other] = false;
                    }
                }
            }

            preferred.at(pad) = source;
            remembered.at(pad) = true;
        }

        // Learn the default mapping once, but do not replace a persistent assignment
        // merely because its preferred source is temporarily unavailable.
        void Observe(std::size_t pad, int source) {
            if (source >= 0 && !remembered.at(pad))
                Remember(pad, source);
        }

        void Reset() {
            preferred.fill(-1);
            remembered.fill(false);
        }

        template <typename Ids, typename TryAssign>
        void Assign(const Ids &pads, TryAssign tryAssign) const {
            std::array<bool, PadCount> assigned{};
            std::array<int, ControllerCount> reservedBy{};
            reservedBy.fill(-1);

            // Reservations survive temporary supported-ID/style restrictions. This prevents
            // another Npad from taking a source whose owner is temporarily absent.
            for (std::size_t pad{}; pad < PadCount; ++pad) {
                if (!remembered[pad])
                    continue;

                const auto source{preferred[pad]};
                if (source >= 0 && static_cast<std::size_t>(source) < ControllerCount)
                    reservedBy[static_cast<std::size_t>(source)] = static_cast<int>(pad);
            }

            for (const auto pad : pads) {
                if (assigned.at(pad) || !remembered.at(pad))
                    continue;

                const auto source{preferred.at(pad)};
                if (source < 0) {
                    // An explicitly empty assignment (for example after swapping into an
                    // unoccupied player) must not be filled by the fallback path.
                    assigned[pad] = true;
                    continue;
                }

                if (static_cast<std::size_t>(source) < ControllerCount)
                    assigned[pad] = tryAssign(pad, static_cast<std::size_t>(source));
            }

            for (const auto pad : pads) {
                if (assigned.at(pad))
                    continue;

                for (std::size_t source{}; source < ControllerCount; ++source) {
                    const auto owner{reservedBy[source]};
                    if (owner >= 0 && static_cast<std::size_t>(owner) != pad)
                        continue;

                    if (tryAssign(pad, source)) {
                        assigned[pad] = true;
                        break;
                    }
                }
            }
        }
    };
}
