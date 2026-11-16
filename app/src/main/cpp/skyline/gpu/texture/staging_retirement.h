// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace skyline::gpu::texture {
    // Counts cache ownership/capacity drops, NOT VMA heap releases or process RSS.
    // Updated only at permanent retirement or existing size-obsolete replacement.
    class StagingRetirementStats {
      private:
        std::atomic<uint64_t> buffers{}, bytes{}, operations{};
        std::string gameName{"unknown"}, titleId{"unknown"};

      public:
        struct Totals {
            uint64_t buffers, bytes, operations;
            bool report{};
        };

        // Set before guest execution; identity remains immutable while workers run.
        void SetIdentity(std::string name, std::string id) {
            gameName = std::move(name);
            titleId = std::move(id);
        }
        const std::string &GameName() const noexcept { return gameName; }
        const std::string &TitleId() const noexcept { return titleId; }

        Totals Snapshot() const noexcept {
            return {buffers.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed),
                    operations.load(std::memory_order_relaxed), false};
        }
        Totals RecordBatch(uint64_t count, uint64_t capacity) noexcept {
            if (!count) return Snapshot();
            buffers.fetch_add(count, std::memory_order_relaxed);
            bytes.fetch_add(capacity, std::memory_order_relaxed);
            const auto sequence{operations.fetch_add(1, std::memory_order_relaxed) + 1};
            auto totals{Snapshot()};
            totals.report = sequence == 1 || sequence % 16 == 0;
            return totals;
        }
    };
}
