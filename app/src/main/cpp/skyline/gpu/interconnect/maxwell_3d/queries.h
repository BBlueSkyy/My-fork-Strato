// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vector>
#include <soc/gm20b/gmmu.h>
#include <gpu/buffer.h>
#include <gpu/interconnect/command_executor.h>
#include "common.h"

namespace skyline::gpu::interconnect::maxwell3d {
    /**
     * @brief Emulates Maxwell sample counters with Vulkan occlusion queries while preserving
     *        counter state across host render-pass and submission boundaries.
     */
    class Queries {
      public:
        enum class CounterType : u32 {
            Occulusion = 0,
            MaxValue
        };

        // Observation-only scene classification. Query segment boundaries
        // remain entirely controlled by the existing Maxwell counter.
        enum class SceneDrawProbe : u8 {
            None = 0,
            DepthPrepass,
            ColorEqual,
            ColorAlways,
            ColorCullNone,
            MaxValue
        };

      private:
        class Counter {
          private:
            static constexpr u32 QueryPoolSize{0x400};

            struct QueryBank;

            struct SegmentRef {
                std::shared_ptr<QueryBank> bank;
                u32 queryIndex;
            };

            struct RenderPassBatch {
                QueryBank *bank;
                u32 firstQuery;
                u32 queryCount;
            };

            GPU &gpu;
            vk::QueryType queryType;
            std::shared_ptr<memory::Buffer> accumulatorBuffer;

            std::vector<std::shared_ptr<QueryBank>> banks;
            std::vector<std::shared_ptr<QueryBank>> executionBanks;
            ContextTag executionTag{};
            u32 executionQueryCount{};

            std::optional<SegmentRef> activeSegment;
            // Observation only: tags the game's existing query segment with
            // scene draws; never changes its begin/end or accumulated report.
            std::array<u32, static_cast<size_t>(SceneDrawProbe::MaxValue)>
                activeSceneModeCounts{};
            u32 activeTotalDrawCount{};
            RenderPassBatch *currentRenderPassBatch{};
            u32 currentRenderPassIndex{std::numeric_limits<u32>::max()};

            std::vector<SegmentRef> pendingSegments;
            bool accumulatorResetPending{true};

            void EnsureExecution(InterconnectContext &ctx);
            std::shared_ptr<QueryBank> AcquireBank(InterconnectContext &ctx, u32 ordinal);
            SegmentRef AllocateSegment(InterconnectContext &ctx);
            void EndActive(InterconnectContext &ctx);
            void ScheduleResolve(InterconnectContext &ctx, std::vector<SegmentRef> segments, bool resetAccumulator,
                                 std::optional<BufferView> reportView = {}, BufferBinding timestampBuffer = {}, bool report64 = false);
            void Compact(InterconnectContext &ctx);

          public:
            Counter(GPU &gpu, vk::QueryType type);
            ~Counter();

            CommandExecutor::SubpassHooks PrepareDraw(
                InterconnectContext &ctx, bool enabled, u32 renderPassIndex,
                SceneDrawProbe observedSceneMode = SceneDrawProbe::None);
            void Report(InterconnectContext &ctx, BufferView view, std::optional<u64> timestamp);
            void Reset(InterconnectContext &ctx);
            void Pause(InterconnectContext &ctx);
            void Flush(InterconnectContext &ctx);
            bool HasActiveSegment() const { return activeSegment.has_value(); }
        };

        std::array<Counter, static_cast<u32>(CounterType::MaxValue)> counters;
        CachedMappedBufferView view{};

      public:
        Queries(GPU &gpu);
        bool HasActiveGuestOcclusion() const {
            return counters[static_cast<u32>(CounterType::Occulusion)].HasActiveSegment();
        }


        CommandExecutor::SubpassHooks PrepareDraw(
            InterconnectContext &ctx, CounterType type, bool enabled,
            u32 renderPassIndex,
            SceneDrawProbe observedSceneMode = SceneDrawProbe::None);

        void Query(InterconnectContext &ctx, soc::gm20b::IOVA address, CounterType type, std::optional<u64> timestamp);
        void ResetCounter(InterconnectContext &ctx, CounterType type);

        /**
         * @brief Ends any active host segment before leaving Maxwell rendering scope.
         */
        void Pause(InterconnectContext &ctx);

        /**
         * @brief Materializes pending segments into the persistent counter accumulator.
         */
        void Flush(InterconnectContext &ctx);
    };
}
