// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <compare>
#include <cstdint>
#include <vector>

namespace skyline::gpu::interconnect {
    // Only values actually queried by shader translation belong here. Dynamic
    // uniform data (including instruction predicates) is not specialization.
    class PipelineSpecialization {
      public:
        // Handles must be checked before querying metadata at an old TIC index.
        enum class Kind : std::uint8_t { ConstantBuffer, TextureType, TextureInteger, SamplerCompare };
        struct Entry {
            Kind kind;
            std::uint32_t stage, index, offset, value;
            auto operator<=>(const Entry &) const = default;
        };

      private:
        std::vector<Entry> entries;

      public:
        void Record(Kind kind, std::uint32_t stage, std::uint32_t index, std::uint32_t offset, std::uint32_t value) {
            Entry entry{kind, stage, index, offset, value};
            const auto position{std::lower_bound(entries.begin(), entries.end(), entry)};
            if (position == entries.end() || *position != entry)
                entries.insert(position, entry);
        }

        template<typename Read>
        bool Matches(Read &&read) const {
            for (const auto &entry : entries)
                if (read(entry) != entry.value)
                    return false;
            return true;
        }

        bool operator==(const PipelineSpecialization &) const = default;
    };
}
