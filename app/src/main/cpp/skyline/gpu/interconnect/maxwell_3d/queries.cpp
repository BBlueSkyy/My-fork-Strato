// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <gpu.h>
#include <soc/gm20b/channel.h>
#include <vulkan/vulkan.hpp>
#include "queries.h"

namespace skyline::gpu::interconnect::maxwell3d {
    struct Queries::Counter::QueryBank {
        vk::raii::QueryPool pool;
        memory::Buffer results;
        DescriptorAllocator::ActiveDescriptorSet resolveDescriptorSet;
        std::weak_ptr<FenceCycle> lastCycle;

        QueryBank(GPU &gpu, vk::QueryType type, memory::Buffer &accumulator)
            : pool{gpu.vkDevice, vk::QueryPoolCreateInfo{
                  .queryType = type,
                  .queryCount = QueryPoolSize,
              }},
              results{gpu.memory.AllocateBuffer(QueryPoolSize * sizeof(u64))},
              resolveDescriptorSet{gpu.helperShaders.queryResolveHelperShader.CreateDescriptorSet(
                  gpu, results.vkBuffer, results.size_bytes(), accumulator.vkBuffer)} {}
    };

    Queries::Counter::Counter(GPU &gpu, vk::QueryType type)
        : gpu{gpu},
          queryType{type},
          accumulatorBuffer{gpu.memory.AllocateBuffer(sizeof(u64))} {}

    Queries::Counter::~Counter() = default;

    void Queries::Counter::EnsureExecution(InterconnectContext &ctx) {
        if (executionTag == ctx.executor.executionTag)
            return;

        executionTag = ctx.executor.executionTag;
        executionQueryCount = 0;
        executionBanks.clear();
        activeSegment.reset();
        currentRenderPassBatch = nullptr;
        currentRenderPassIndex = std::numeric_limits<u32>::max();
    }

    std::shared_ptr<Queries::Counter::QueryBank> Queries::Counter::AcquireBank(InterconnectContext &ctx, u32 ordinal) {
        EnsureExecution(ctx);

        while (executionBanks.size() <= ordinal) {
            std::shared_ptr<QueryBank> selected;

            for (auto &bank : banks) {
                if (std::find(executionBanks.begin(), executionBanks.end(), bank) != executionBanks.end())
                    continue;

                auto lastCycle{bank->lastCycle.lock()};
                if (!lastCycle || lastCycle->Poll(false)) {
                    selected = bank;
                    break;
                }
            }

            if (!selected) {
                selected = std::make_shared<QueryBank>(gpu, queryType, accumulatorBuffer);
                banks.emplace_back(selected);
            }

            selected->lastCycle = ctx.executor.cycle;
            ctx.executor.AttachDependency(selected);

            vk::QueryPool queryPool{*selected->pool};
            ctx.executor.InsertPreExecuteCommand([queryPool](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
                commandBuffer.resetQueryPool(queryPool, 0, QueryPoolSize);
            });

            executionBanks.emplace_back(selected);
        }

        return executionBanks[ordinal];
    }

    Queries::Counter::SegmentRef Queries::Counter::AllocateSegment(InterconnectContext &ctx) {
        EnsureExecution(ctx);

        const u32 linearIndex{executionQueryCount++};
        const u32 bankOrdinal{linearIndex / QueryPoolSize};
        const u32 queryIndex{linearIndex % QueryPoolSize};

        return {AcquireBank(ctx, bankOrdinal), queryIndex};
    }

    void Queries::Counter::EndActive(InterconnectContext &ctx) {
        if (!activeSegment)
            return;

        auto active{std::move(*activeSegment)};
        activeSegment.reset();

        ctx.executor.AddCommand([bank = std::move(active.segment.bank), queryIndex = active.segment.queryIndex]
                                (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
            commandBuffer.endQuery(*bank->pool, queryIndex);
        });
    }

    CommandExecutor::SubpassHooks Queries::Counter::PrepareDraw(InterconnectContext &ctx, bool enabled, u32 renderPassIndex) {
        EnsureExecution(ctx);

        if (!enabled) {
            EndActive(ctx);
            return {};
        }

        if (activeSegment)
            return {};

        auto segment{AllocateSegment(ctx)};
        pendingSegments.emplace_back(segment);

        if (!currentRenderPassBatch ||
            currentRenderPassIndex != renderPassIndex ||
            currentRenderPassBatch->bank != segment.bank.get()) {
            currentRenderPassIndex = renderPassIndex;
            currentRenderPassBatch = ctx.executor.allocator->EmplaceUntracked<RenderPassBatch>(
                RenderPassBatch{segment.bank.get(), segment.queryIndex, 0});

            auto *batch{currentRenderPassBatch};
            auto bank{segment.bank};
            ctx.executor.InsertPostRpCommand([bank = std::move(bank), batch]
                                             (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
                if (!batch->queryCount)
                    return;

                commandBuffer.copyQueryPoolResults(
                    *bank->pool,
                    batch->firstQuery,
                    batch->queryCount,
                    bank->results.vkBuffer,
                    static_cast<vk::DeviceSize>(batch->firstQuery) * sizeof(u64),
                    sizeof(u64),
                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
            });
        }

        auto *batch{currentRenderPassBatch};
        batch->queryCount++;
        activeSegment = ActiveSegment{segment, batch};

        vk::QueryControlFlags flags{};
        if (gpu.traits.supportsOcclusionQueryPrecise)
            flags |= vk::QueryControlFlagBits::ePrecise;

        CommandExecutor::SubpassHooks hooks{};
        hooks.before = [bank = std::move(segment.bank), queryIndex = segment.queryIndex, flags]
                       (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32) {
            commandBuffer.beginQuery(*bank->pool, queryIndex, flags);
        };
        return hooks;
    }

    void Queries::Counter::ScheduleResolve(InterconnectContext &ctx, std::vector<SegmentRef> segments, bool resetAccumulator,
                                           std::optional<BufferView> reportView, BufferBinding timestampBuffer, bool report64) {
        ctx.executor.AddPostRpCommand(
            [this, segments = std::move(segments), resetAccumulator, reportView, timestampBuffer, report64]
            (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) mutable {
                bool reset{resetAccumulator};

                size_t offset{};
                while (offset < segments.size()) {
                    auto &firstSegment{segments[offset]};
                    const u32 firstQuery{firstSegment.queryIndex};
                    u32 count{1};

                    while (offset + count < segments.size() &&
                           segments[offset + count].bank.get() == firstSegment.bank.get() &&
                           segments[offset + count].queryIndex == firstQuery + count)
                        count++;

                    gpu.helperShaders.queryResolveHelperShader.Resolve(
                        commandBuffer,
                        *firstSegment.bank->resolveDescriptorSet,
                        firstQuery,
                        count,
                        reset);

                    reset = false;
                    offset += count;
                }

                if (segments.empty() && reset)
                    commandBuffer.fillBuffer(accumulatorBuffer.vkBuffer, 0, sizeof(u64), 0);

                if (!reportView)
                    return;

                gpu.helperShaders.queryResolveHelperShader.PrepareForTransfer(commandBuffer);

                auto dstBinding{reportView->GetBinding(gpu)};

                if (timestampBuffer) {
                    commandBuffer.copyBuffer(timestampBuffer.buffer, dstBinding.buffer, vk::BufferCopy{
                        .srcOffset = timestampBuffer.offset,
                        .dstOffset = dstBinding.offset + sizeof(u64),
                        .size = sizeof(u64),
                    });

                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eTransfer,
                        {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        },
                        {}, {});
                }

                commandBuffer.copyBuffer(accumulatorBuffer.vkBuffer, dstBinding.buffer, vk::BufferCopy{
                    .srcOffset = 0,
                    .dstOffset = dstBinding.offset,
                    .size = report64 ? sizeof(u64) : sizeof(u32),
                });
            });
    }

    void Queries::Counter::Compact(InterconnectContext &ctx) {
        EndActive(ctx);

        if (pendingSegments.empty())
            return;

        auto segments{std::move(pendingSegments)};
        pendingSegments.clear();

        const bool resetAccumulator{accumulatorResetPending};
        accumulatorResetPending = false;
        ScheduleResolve(ctx, std::move(segments), resetAccumulator);
    }

    void Queries::Counter::Report(InterconnectContext &ctx, BufferView reportView, std::optional<u64> timestamp) {
        EndActive(ctx);

        auto segments{std::move(pendingSegments)};
        pendingSegments.clear();

        const bool resetAccumulator{accumulatorResetPending};
        accumulatorResetPending = false;

        BufferBinding timestampBuffer{};
        if (timestamp)
            timestampBuffer = ctx.gpu.megaBufferAllocator.Push(ctx.executor.cycle, span<u64>(*timestamp).cast<u8>());

        ScheduleResolve(ctx, std::move(segments), resetAccumulator, reportView, timestampBuffer, timestamp.has_value());
    }

    void Queries::Counter::Reset(InterconnectContext &ctx) {
        EndActive(ctx);
        pendingSegments.clear();
        accumulatorResetPending = true;
    }

    void Queries::Counter::Pause(InterconnectContext &ctx) {
        EndActive(ctx);
    }

    void Queries::Counter::Flush(InterconnectContext &ctx) {
        Compact(ctx);
    }

    Queries::Queries(GPU &gpu) : counters{{{gpu, vk::QueryType::eOcclusion}}} {}

    CommandExecutor::SubpassHooks Queries::PrepareDraw(InterconnectContext &ctx, CounterType type, bool enabled, u32 renderPassIndex) {
        return counters[static_cast<u32>(type)].PrepareDraw(ctx, enabled, renderPassIndex);
    }

    void Queries::Query(InterconnectContext &ctx, soc::gm20b::IOVA address, CounterType type, std::optional<u64> timestamp) {
        view.Update(ctx, address, timestamp ? 16 : 4);
        if (!view.view)
            return;

        usedQueryAddresses.emplace(u64{address});
        ctx.executor.AttachBuffer(*view);

        view->GetBuffer()->BlockSequencedCpuBackingWrites();
        view->GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);

        counters[static_cast<u32>(type)].Report(ctx, *view, timestamp);
    }

    void Queries::ResetCounter(InterconnectContext &ctx, CounterType type) {
        counters[static_cast<u32>(type)].Reset(ctx);
    }

    void Queries::Pause(InterconnectContext &ctx) {
        for (auto &counter : counters)
            counter.Pause(ctx);
    }

    void Queries::Flush(InterconnectContext &ctx) {
        for (auto &counter : counters)
            counter.Flush(ctx);
        view.PurgeCaches();
    }

    bool Queries::QueryPresentAtAddress(soc::gm20b::IOVA address) {
        return usedQueryAddresses.contains(u64{address});
    }
}
