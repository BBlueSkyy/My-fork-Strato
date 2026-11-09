// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <gpu/diagnostic/eow_capture.h>
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
          accumulatorBuffer{std::make_shared<memory::Buffer>(gpu.memory.AllocateBuffer(sizeof(u64)))} {}

    Queries::Counter::~Counter() = default;

    void Queries::Counter::EnsureExecution(InterconnectContext &ctx) {
        if (executionTag == ctx.executor.executionTag)
            return;

        executionTag = ctx.executor.executionTag;
        executionQueryCount = 0;
        executionBanks.clear();
        activeSegment.reset();
        activeSceneModeCounts.fill(0);
        activeTotalDrawCount = 0;
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
        const auto modes{std::exchange(activeSceneModeCounts,
                                       decltype(activeSceneModeCounts){})};
        const u32 guestDraws{std::exchange(activeTotalDrawCount, 0U)};
        const u32 depthDraws{modes[static_cast<size_t>(SceneDrawProbe::DepthPrepass)]};
        const u32 equalDraws{modes[static_cast<size_t>(SceneDrawProbe::ColorEqual)]};
        const u32 alwaysDraws{modes[static_cast<size_t>(SceneDrawProbe::ColorAlways)]};
        const u32 cullNoneDraws{modes[static_cast<size_t>(SceneDrawProbe::ColorCullNone)]};
        const u32 combinedDraws{modes[static_cast<size_t>(SceneDrawProbe::ColorCombined)]};
        const u32 sceneDraws{depthDraws + equalDraws + alwaysDraws +
                             cullNoneDraws + combinedDraws};

        // Preserve the original NVN/guest query exactly. The opt-in EOW probe
        // reads the SAME segment only after endQuery and the natural RP end.
        ctx.executor.AddCommand([bank = active.bank, queryIndex = active.queryIndex]
                                (vk::raii::CommandBuffer &commandBuffer,
                                 const std::shared_ptr<FenceCycle> &, GPU &) {
            commandBuffer.endQuery(*bank->pool, queryIndex);
        });

        auto &cap{diagnostic::EowCapture()};
        if (!cap.active.load(std::memory_order_acquire) ||
            !cap.sceneOcclusionEnabled.load(std::memory_order_relaxed) ||
            !sceneDraws)
            return;

        // The game's segment can include several types of draws: DO NOT
        // attribute a mixed-segment sample count to one changed pipeline.
        const bool depthOnly{depthDraws && !equalDraws && !alwaysDraws &&
                             !cullNoneDraws};
        const u32 activeClasses{static_cast<u32>(depthDraws != 0) +
                                static_cast<u32>(equalDraws != 0) +
                                static_cast<u32>(alwaysDraws != 0) +
                                static_cast<u32>(cullNoneDraws != 0) +
                                static_cast<u32>(combinedDraws != 0)};
        // An apparently single scene mode can still share the guest query
        // with non-scene draws. Never attribute that entire sample count
        // exclusively to the scene pipeline.
        const bool mixedSegment{activeClasses > 1 || guestDraws != sceneDraws};
        const char *role{mixedSegment ? "scene_mixed" :
                         depthOnly ? "scene_depth_prepass" :
                         equalDraws ? "scene_color_equal" :
                         alwaysDraws ? "scene_color_always" :
                         cullNoneDraws ? "scene_color_cull_none_equal" :
                                         "scene_color_always_cull_none"};
        const auto ticket{cap.ReserveOcclusionQuery(depthOnly)};
        if (!ticket) return;
        const u32 id{*ticket};
        try {
            auto staging{gpu.memory.AllocateStagingBuffer(2 * sizeof(u64))};
            auto bank{active.bank};
            const auto sourceIndex{active.queryIndex};
            const auto path{std::filesystem::path(cap.CaptureDirectory()) /
                fmt::format("occlusion_{:02}.txt", id)};
            const auto meta{fmt::format(
                "id={} role={} source=guest_query_segment submit={} rp={} query_index={} "
                "scene_draws={} guest_draws={} scene_equal_draws={} "
                "scene_always_draws={} scene_cull_none_draws={} "
                "scene_combined_draws={} scene_depth_draws={} mixed={} precise={} "
                "encoding=uint64_samples,uint64_available",
                id, role,
                ctx.executor.submissionNumber, currentRenderPassIndex,
                sourceIndex, sceneDraws, guestDraws, equalDraws,
                alwaysDraws, cullNoneDraws, combinedDraws, depthDraws, mixedSegment,
                gpu.traits.supportsOcclusionQueryPrecise)};
            // The existing post-RP copy into the game's results buffer stays
            // untouched. This second copy only feeds a diagnostic staging
            // buffer and does not allocate, reset or nest another query.
            ctx.executor.InsertPostRpCommand(
                [bank, staging, sourceIndex](
                    vk::raii::CommandBuffer &commandBuffer,
                    const std::shared_ptr<FenceCycle> &cycle, GPU &) {
                    cycle->AttachObjects(bank, staging);
                    commandBuffer.copyQueryPoolResults(
                        *bank->pool, sourceIndex, 1,
                        staging->vkBuffer, 0, 2 * sizeof(u64),
                        vk::QueryResultFlagBits::e64 |
                        vk::QueryResultFlagBits::eWithAvailability);
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eHost, {}, {},
                        vk::BufferMemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                            .dstAccessMask = vk::AccessFlagBits::eHostRead,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .buffer = staging->vkBuffer,
                            .offset = 0,
                            .size = staging->size(),
                        }, {});
                });
            ctx.executor.QueueEowReadbackCallback(
                [staging, meta, path] {
                    std::array<u64, 2> values{};
                    std::memcpy(values.data(), staging->data(), sizeof(values));
                    const bool available{values[1] != 0};
                    std::ofstream out(path, std::ios::trunc);
                    if (out)
                        out << meta << "\n"
                            << "samples=" << (available ? values[0] : 0) << "\n"
                            << "available=" << available << "\n";
                    out.close();
                    diagnostic::EowCapture().OcclusionQueryFinished(out.good(),
                                                                    available);
                });
            cap.Record(fmt::format(
                "GUEST_OCCLUSION_READBACK_QUEUED id={} role={} submit={} rp={} query_index={} "
                "scene_draws={} guest_draws={} equal={} always={} cull_none={} "
                "combined={} depth={} mixed={} source=existing_guest_query",
                id, role,
                ctx.executor.submissionNumber, currentRenderPassIndex,
                sourceIndex, sceneDraws, guestDraws, equalDraws, alwaysDraws,
                cullNoneDraws, combinedDraws, depthDraws, mixedSegment));
        } catch (const std::exception &e) {
            cap.Record(fmt::format(
                "GUEST_OCCLUSION_READBACK_ERROR id={} reason={}", id, e.what()));
            cap.OcclusionQueryFinished(false, false);
        }
    }

    CommandExecutor::SubpassHooks Queries::Counter::PrepareDraw(
        InterconnectContext &ctx, bool enabled, u32 renderPassIndex,
        SceneDrawProbe observedSceneMode) {
        EnsureExecution(ctx);

        if (!enabled) {
            EndActive(ctx);
            return {};
        }

        if (activeSegment) {
            ++activeTotalDrawCount;
            if (observedSceneMode != SceneDrawProbe::None &&
                observedSceneMode != SceneDrawProbe::MaxValue)
                ++activeSceneModeCounts[static_cast<size_t>(observedSceneMode)];
            return {};
        }

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
        activeSceneModeCounts.fill(0);
        activeTotalDrawCount = 1;
        if (observedSceneMode != SceneDrawProbe::None &&
            observedSceneMode != SceneDrawProbe::MaxValue)
            ++activeSceneModeCounts[static_cast<size_t>(observedSceneMode)];

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
        // The channel's Maxwell state can be destroyed before its executor drains.
        // Keep the accumulator alive through both deferred recording and GPU use.
        ctx.executor.AttachDependency(accumulatorBuffer);
        ctx.executor.AddPostRpCommand(
            [accumulator = accumulatorBuffer, segments = std::move(segments), resetAccumulator, reportView, timestampBuffer, report64]
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

                commandBuffer.copyBuffer(accumulator->vkBuffer, dstBinding.buffer, vk::BufferCopy{
                    .srcOffset = 0,
                    .dstOffset = dstBinding.offset,
                    .size = report64 ? sizeof(u64) : sizeof(u32),
                });

                reportBarrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                reportBarrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                              vk::PipelineStageFlagBits::eAllCommands,
                                              {}, {}, reportBarrier, {});
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

    CommandExecutor::SubpassHooks Queries::PrepareDraw(
        InterconnectContext &ctx, CounterType type, bool enabled,
        u32 renderPassIndex, SceneDrawProbe observedSceneMode) {
        return counters[static_cast<u32>(type)].PrepareDraw(
            ctx, enabled, renderPassIndex, observedSceneMode);
    }

    void Queries::Query(InterconnectContext &ctx, soc::gm20b::IOVA address, CounterType type, std::optional<u64> timestamp) {
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
