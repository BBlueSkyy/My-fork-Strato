// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
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

            struct ActiveSegment {
                SegmentRef segment;
                RenderPassBatch *batch;
            };

            GPU &gpu;
            vk::QueryType queryType;
            memory::Buffer accumulatorBuffer;

            std::vector<std::shared_ptr<QueryBank>> banks;
            std::vector<std::shared_ptr<QueryBank>> executionBanks;
            ContextTag executionTag{};
            u32 executionQueryCount{};

            std::optional<ActiveSegment> activeSegment;
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

            CommandExecutor::SubpassHooks PrepareDraw(InterconnectContext &ctx, bool enabled, u32 renderPassIndex);
            void Report(InterconnectContext &ctx, BufferView view, std::optional<u64> timestamp);
            void Reset(InterconnectContext &ctx);
            void Pause(InterconnectContext &ctx);
            void Flush(InterconnectContext &ctx);
        };

        std::array<Counter, static_cast<u32>(CounterType::MaxValue)> counters;
        CachedMappedBufferView view{};
        std::unordered_set<u64> usedQueryAddresses;

      public:
        Queries(GPU &gpu);

        CommandExecutor::SubpassHooks PrepareDraw(InterconnectContext &ctx, CounterType type, bool enabled, u32 renderPassIndex);

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

        bool QueryPresentAtAddress(soc::gm20b::IOVA address);
    };
}
