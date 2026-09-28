// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>

namespace skyline::input {
    template <std::size_t PadCount, std::size_t ControllerCount>
    class NpadAssignmentOrder {
        std::array<int, PadCount> preferred{};

      public:
        NpadAssignmentOrder() { Reset(); }

        void Remember(std::size_t pad, int source) { preferred.at(pad) = source; }
        // A temporary style or ID restriction disconnects the Npad without changing its assignment.
        void Observe(std::size_t pad, int source) {
            if (source >= 0)
                Remember(pad, source);
        }
        void Reset() { preferred.fill(-1); }

        template <typename Ids, typename TryAssign>
        void Assign(const Ids &pads, TryAssign tryAssign) const {
            std::array<bool, PadCount> assigned{};

            // Reserve existing bindings before assigning any unbound player.
            for (const auto pad : pads) {
                if (assigned.at(pad))
                    continue;
                const auto source{preferred.at(pad)};
                if (source >= 0 && static_cast<std::size_t>(source) < ControllerCount)
                    assigned[pad] = tryAssign(pad, static_cast<std::size_t>(source));
            }

            for (const auto pad : pads) {
                if (assigned.at(pad))
                    continue;
                for (std::size_t source{}; source < ControllerCount; ++source) {
                    if (tryAssign(pad, source)) {
                        assigned[pad] = true;
                        break;
                    }
                }
            }
        }
    };
}
