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
        enum class Kind : std::uint8_t { ConstantBuffer, TextureType, TextureInteger, SamplerCompare, TextureBufferType };
        struct TextureSource {
            std::uint32_t cbuf_index, cbuf_offset, shift_left;
            std::uint32_t secondary_cbuf_index, secondary_cbuf_offset, secondary_shift_left;
            auto operator<=>(const TextureSource &) const = default;
            template<typename Read>
            std::uint32_t ReadHandle(Read &&read) const {
                const auto primary{read(cbuf_index, cbuf_offset)};
                const auto secondary{cbuf_index == secondary_cbuf_index && cbuf_offset == secondary_cbuf_offset ? primary : read(secondary_cbuf_index, secondary_cbuf_offset)};
                return (primary << shift_left) | (secondary << secondary_shift_left);
            }
        };
        struct TextureQuery {
            std::uint32_t stage;
            TextureSource source;
            Kind kind;
            std::uint32_t value;
            auto operator<=>(const TextureQuery &) const = default;
        };
        struct Entry {
            Kind kind;
            std::uint32_t stage, index, offset, value;
            auto operator<=>(const Entry &) const = default;
        };

      private:
        std::vector<Entry> entries;
        std::vector<TextureQuery> textureQueries;

      public:
        void Record(Kind kind, std::uint32_t stage, std::uint32_t index, std::uint32_t offset, std::uint32_t value) {
            Entry entry{kind, stage, index, offset, value};
            const auto position{std::lower_bound(entries.begin(), entries.end(), entry)};
            if (position == entries.end() || *position != entry)
                entries.insert(position, entry);
        }

        template<typename Source>
        void RecordTexture(Kind kind, std::uint32_t stage, const Source &source, std::uint32_t value) {
            TextureQuery query{stage, {source.cbuf_index, source.cbuf_offset, source.shift_left,
                                      source.secondary_cbuf_index, source.secondary_cbuf_offset, source.secondary_shift_left}, kind, value};
            const auto position{std::lower_bound(textureQueries.begin(), textureQueries.end(), query)};
            if (position == textureQueries.end() || *position != query)
                textureQueries.insert(position, query);
        }

        template<typename Read, typename Handle, typename Metadata>
        bool Matches(Read &&read, Handle &&readHandle, Metadata &&metadata) const {
            // Scalar CBUF words were actually folded into code/control flow.
            for (const auto &entry : entries)
                if (read(entry) != entry.value)
                    return false;
            // A changed handle is allowed when the queried metadata is equal.
            // Group queries by source: one word read for type/int/compare together.
            const TextureQuery *previous{};
            std::uint32_t handle{};
            for (const auto &query : textureQueries) {
                if (!previous || previous->stage != query.stage || previous->source != query.source)
                    handle = readHandle(query.stage, query.source);
                if (metadata(query.kind, handle) != query.value)
                    return false;
                previous = &query;
            }
            return true;
        }

        template<typename Read>
        bool Matches(Read &&read) const {
            for (const auto &entry : entries)
                if (read(entry) != entry.value)
                    return false;
            return textureQueries.empty();
        }

        bool operator==(const PipelineSpecialization &) const = default;
    };
}
