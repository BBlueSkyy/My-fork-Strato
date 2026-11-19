// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <gpu.h>
#include <soc/gm20b/channel.h>
#include <vulkan/vulkan.hpp>
#include "queries.h"
#if STRATO_OCEAN_VIS_MODE > 0
#include <logger/logger.h>
#endif

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
          accumulatorBuffer{std::make_shared<memory::Buffer>(gpu.memory.AllocateBuffer(sizeof(u64)))} {}

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
                selected = std::make_shared<QueryBank>(gpu, queryType, *accumulatorBuffer);
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

        ctx.executor.AddCommand([bank = std::move(active.bank), queryIndex = active.queryIndex]
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
        activeSegment = segment;

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
                                           std::optional<BufferView> reportView, BufferBinding timestampBuffer, bool report64
#if STRATO_OCEAN_VIS_MODE > 0
                                           , std::shared_ptr<memory::Buffer> diagnosticSample
#endif
                                           ) {
        // The channel's Maxwell state can be destroyed before its executor drains.
        // Keep the accumulator alive through both deferred recording and GPU use.
        ctx.executor.AttachDependency(accumulatorBuffer);
        ctx.executor.AddPostRpCommand(
            [accumulator = accumulatorBuffer, segments = std::move(segments), resetAccumulator, reportView, timestampBuffer, report64
#if STRATO_OCEAN_VIS_MODE > 0
             , diagnosticSample = std::move(diagnosticSample)
#endif
             ]
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

                if (segments.empty() && reset) {
                    // A reset may follow a compute accumulation or a report reading
                    // the accumulator in this same submission (WAW / WAR).
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eTransfer,
                        {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
                            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        }, {}, {});
                    commandBuffer.fillBuffer(accumulator->vkBuffer, 0, sizeof(u64), 0);
                }

                if (!reportView)
                    return;

                gpu.helperShaders.queryResolveHelperShader.PrepareForTransfer(commandBuffer);

                auto dstBinding{reportView->GetBinding(gpu)};

                // A report may replace data used by an earlier draw, conditional
                // predicate or DMA command. Scope the dependency to the report,
                // rather than serializing unrelated buffers or graphics work.
                vk::BufferMemoryBarrier reportBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dstBinding.buffer,
                    .offset = dstBinding.offset,
                    .size = report64 ? sizeof(u64) * 2 : sizeof(u32),
                };
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                              vk::PipelineStageFlagBits::eTransfer,
                                              {}, {}, reportBarrier, {});

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

#if STRATO_OCEAN_VIS_MODE > 0
                const vk::DeviceSize reportBytes{report64 ? sizeof(u64) : sizeof(u32)};
                if (diagnosticSample) {
                    // GPU snapshots the genuine accumulated result before guest override.
                    commandBuffer.copyBuffer(accumulator->vkBuffer, diagnosticSample->vkBuffer,
                                             vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = reportBytes});
                }
#endif
#if STRATO_OCEAN_VIS_MODE == 2
                // A/B ONLY: report positive occlusion in guest GPU memory.
                // Keep real counter/accumulator, predicate state and timestamp intact.
                commandBuffer.fillBuffer(dstBinding.buffer, dstBinding.offset, sizeof(u32), 1);
                if (report64)
                    commandBuffer.fillBuffer(dstBinding.buffer, dstBinding.offset + sizeof(u32), sizeof(u32), 0);
#else
                commandBuffer.copyBuffer(accumulator->vkBuffer, dstBinding.buffer, vk::BufferCopy{
                    .srcOffset = 0,
                    .dstOffset = dstBinding.offset,
                    .size = report64 ? sizeof(u64) : sizeof(u32),
                });
#endif

                reportBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                reportBarrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                              vk::PipelineStageFlagBits::eAllCommands,
                                              {}, {}, reportBarrier, {});
#if STRATO_OCEAN_VIS_MODE > 0
                if (diagnosticSample) {
                    // Existing report barrier orders the read from guest output.
                    commandBuffer.copyBuffer(dstBinding.buffer, diagnosticSample->vkBuffer, vk::BufferCopy{
                        .srcOffset = dstBinding.offset, .dstOffset = sizeof(u64), .size = reportBytes,
                    });
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                            .dstAccessMask = vk::AccessFlagBits::eHostRead,
                        }, {}, {});
                }
#endif
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

        // A report is a guest memory write at this point in the command stream.
        // Deferring it past later draws would let vertex/SSBO/indirect readers see
        // the old value. Vulkan query copies and the compute resolve require an
        // outside-render-pass scope; close the pass, without submitting the slot.
        ctx.executor.BreakRenderPass();

        auto segments{std::move(pendingSegments)};
        pendingSegments.clear();

        const bool resetAccumulator{accumulatorResetPending};
        accumulatorResetPending = false;

        BufferBinding timestampBuffer{};
        if (timestamp)
            timestampBuffer = ctx.gpu.megaBufferAllocator.Push(ctx.executor.cycle, span<u64>(*timestamp).cast<u8>());

#if STRATO_OCEAN_VIS_MODE > 0
        // Oceanhorn makes >150k query reports per session: sample sparsely.
        ++diagnosticReportCount;
        std::shared_ptr<memory::Buffer> diagnosticSample;
        if (diagnosticReportCount == 1 || (diagnosticReportCount % 8192) == 0) {
            diagnosticSample = std::make_shared<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(2 * sizeof(u64)));
            std::memset(diagnosticSample->data(), 0, 2 * sizeof(u64));
            ctx.executor.AttachDependency(diagnosticSample);
            const auto reportSerial{diagnosticReportCount};
            const auto segmentCount{segments.size()};
            const auto wasReset{resetAccumulator};
            const auto report64{timestamp.has_value()};
            const auto cycle{ctx.executor.cycle};
            // GPU completion is required before host reads coherent mapped samples.
            // Only a sparse subset incurs a wait when direct import is disabled.
            ctx.executor.AddDeferredAction([cycle, diagnosticSample, reportSerial, segmentCount, wasReset, report64] {
                cycle->Wait();
                u64 realSamples{}, delivered{};
                const size_t bytes{report64 ? sizeof(u64) : sizeof(u32)};
                std::memcpy(&realSamples, diagnosticSample->data(), bytes);
                std::memcpy(&delivered, diagnosticSample->data() + sizeof(u64), bytes);
                LOGI("[OCEAN-QRESULT] serial={} gpu_samples={} guest_result={} segments={} reset={} bits={} forced={}",
                     reportSerial, realSamples, delivered, segmentCount, wasReset,
                     report64 ? 64 : 32, STRATO_OCEAN_VIS_MODE == 2);
            });
        }
#if STRATO_OCEAN_VIS_MODE == 2
        if (diagnosticReportCount == 1)
            LOGI("[OCEAN-QUERY-FORCED-POSITIVE] Enabled; changes guest report bytes only, not counters or timestamps");
#endif
#endif
        ScheduleResolve(ctx, std::move(segments), resetAccumulator, reportView, timestampBuffer, timestamp.has_value()
#if STRATO_OCEAN_VIS_MODE > 0
                        , std::move(diagnosticSample)
#endif
                        );
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
#if STRATO_OCEAN_VIS_MODE > 0
        static thread_local u64 reports{};
        ++reports;
        if (reports == 1 || (reports % 8192) == 0)
            LOGI("[OCEAN-QREPORT] sample-reports={} address=0x{:X} counter={} timestamp={}",
                 reports, u64{address}, static_cast<u32>(type), timestamp.has_value());
#endif
        view.Update(ctx, address, timestamp ? 16 : 4);
        if (!view.view)
            return;

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
}
