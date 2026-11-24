// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace skyline::applet::swkbd {
    /** Temporary diagnostics: repeated Calc/poll signatures never produce per-frame logs. */
    class SoftwareKeyboardTraceGate {
      public:
        enum class Point { NormalInput, Request, Calc, Config, Reply, TextReply, Frontend, Rejected, CalcComplete, InputFields, Common };
        enum class Decision { Emit, Duplicate, Limit, Suppressed };
        static constexpr std::size_t Capacity{64};

        Decision Take(Point point, std::array<std::uint64_t, 4> values = {}) {
            std::scoped_lock lock{mutex};
            for (std::size_t index{}; index < count; ++index)
                if (keys[index].point == point && keys[index].values == values)
                    return Decision::Duplicate;
            if (count == Capacity) {
                if (limitReported)
                    return Decision::Suppressed;
                limitReported = true;
                return Decision::Limit;
            }
            keys[count++] = {point, values};
            return Decision::Emit;
        }

      private:
        struct Key { Point point{}; std::array<std::uint64_t, 4> values{}; };
        std::mutex mutex;
        std::array<Key, Capacity> keys{};
        std::size_t count{};
        bool limitReported{};
    };
}
