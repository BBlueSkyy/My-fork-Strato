// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <gpu.h>
#include <soc/gm20b/channel.h>
#include <vulkan/vulkan.hpp>
#include "queries.h"

namespace skyline::gpu::interconnect::maxwell3d {
    Queries::Counter::Counter(vk::raii::Device &device, vk::QueryType type) : pool{device, vk::QueryPoolCreateInfo{
        .queryType = type,
        .queryCount = Counter::QueryPoolSize
    }} {}

    std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> Queries::Counter::Prepare(InterconnectContext &ctx) {
        auto currentRenderPassIndex{*ctx.executor.GetRenderPassIndex()};
        if (ctx.executor.executionTag != lastTag || lastRenderPassIndex != currentRenderPassIndex) {
            lastTag = ctx.executor.executionTag;
            lastRenderPassIndex = currentRenderPassIndex;
            diagnosticRenderPasses++;

            // Allocate per-RP memory for tracking queries
            queries = ctx.executor.allocator->AllocateUntracked<Query>(Counter::QueryPoolSize);
            usedQueryCount = ctx.executor.allocator->EmplaceUntracked<u32>();
            queryActive = ctx.executor.allocator->EmplaceUntracked<bool>();
            std::memset(queries.data(), 0, queries.size_bytes());

            recordOnNextEnd = true;

            // Reset the query pool up to the final used query count before the current RP begins
            return [this, usedQueryCountPtr = this->usedQueryCount](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
                commandBuffer.resetQueryPool(*pool, 0, *usedQueryCountPtr);
            };
        }

        return {};
    }

    //TODO call cmdbuf begin
    void Queries::Counter::Begin(InterconnectContext &ctx, bool atExecutionStart) {
        auto prepareFunc{Prepare(ctx)};

        *queryActive = true;
        (*usedQueryCount)++;
        diagnosticBegins++;
        diagnosticMaxQueriesPerRenderPass = std::max(diagnosticMaxQueriesPerRenderPass, *usedQueryCount);
        if (*usedQueryCount > QueryPoolSize)
            diagnosticPoolOverflowAttempts++;

        // Begin the query with the current query count as index
        auto func{[this, queryIndex = *this->usedQueryCount - 1](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
            commandBuffer.beginQuery(*pool, queryIndex, vk::QueryControlFlagBits::ePrecise);
        }};

        if (atExecutionStart) {
            ctx.executor.InsertPreExecuteCommand(std::move(func));

            if (prepareFunc)
                ctx.executor.InsertPreExecuteCommand(std::move(prepareFunc));
        } else {
            if (prepareFunc)
                ctx.executor.InsertPreRpCommand(std::move(prepareFunc));

            ctx.executor.AddCommand(std::move(func));
        }
    }

    // TODO must be called after begin in cmdbuf
    void Queries::Counter::Report(InterconnectContext &ctx, BufferView view, std::optional<u64> timestamp) {
        diagnosticReports++;
        diagnosticResultCopies++;
        if (timestamp)
            diagnosticTimestampCopies++;

        if (ctx.executor.executionTag != lastTag)
            Begin(ctx, true);

        // End the query with the current query count as index
        ctx.executor.AddCommand([=, this, queryIndex = *this->usedQueryCount - 1](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
            commandBuffer.endQuery(*pool, queryIndex);
        });

        *queryActive = false;

        // Allocate memory for the timestamp in the megabuffer since updateBuffer can be expensive
        BufferBinding timestampBuffer{timestamp ? ctx.gpu.megaBufferAllocator.Push(ctx.executor.cycle, span<u64>(*timestamp).cast<u8>()) : BufferBinding{}};
        queries[*usedQueryCount - 1] = {view, timestampBuffer};

        if (recordOnNextEnd) {
            diagnosticPostRpBatches++;
            ctx.executor.InsertPostRpCommand([this, queriesPtr = this->queries, usedQueryCountPtr = this->usedQueryCount, queryActivePtr = this->queryActive](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                if (*queryActivePtr)
                    commandBuffer.endQuery(*pool, *usedQueryCountPtr - 1);

                for (u32 i{}; i < *usedQueryCountPtr; i++) {
                    if (!queriesPtr[i].view)
                        continue;

                    auto dstBinding{queriesPtr[i].view.GetBinding(gpu)};
                    auto timestampSrcBinding{queriesPtr[i].timestampBinding};

                    commandBuffer.copyQueryPoolResults(*pool, i, 1, dstBinding.buffer, dstBinding.offset, 0, {});
                    if (timestampSrcBinding)
                        commandBuffer.copyBuffer(timestampSrcBinding.buffer, dstBinding.buffer, {vk::BufferCopy{
                            .size = 8,
                            .srcOffset = timestampSrcBinding.offset,
                            .dstOffset = dstBinding.offset + 8
                        }});
                }
            });
            recordOnNextEnd = false;
        }
    }

    // TODO must be called after begin in cmdbuf
    // TODO call at exec end
    void Queries::Counter::End(InterconnectContext &ctx) {
        if (ctx.executor.executionTag != lastTag  || !queryActive || !*queryActive)
            return;

        diagnosticEnds++;

        // End the query with the current query count as index
        ctx.executor.AddCommand([=, this, queryIndex = *this->usedQueryCount - 1](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
            commandBuffer.endQuery(*pool, queryIndex);
        });

        *queryActive = false;
    }

    void Queries::Counter::RecordReset() {
        diagnosticResets++;
    }

    void Queries::Counter::RecordBufferAttachment(bool newlyAttached) {
        if (newlyAttached)
            diagnosticBufferAttachments++;
        else
            diagnosticBufferReuses++;
    }

    void Queries::Counter::LogAndResetDiagnostics(size_t submissionNumber) {
        if (diagnosticReports || diagnosticBegins || diagnosticEnds || diagnosticResets) {
            LOGI("GPU_QUERY_DIAG submission={} reports={} begins={} ends={} resets={} render_passes={} max_queries_per_rp={} post_rp_batches={} result_copies={} timestamp_copies={} buffer_attaches={} buffer_reuses={} pool_overflow_attempts={}",
                 submissionNumber, diagnosticReports, diagnosticBegins, diagnosticEnds, diagnosticResets,
                 diagnosticRenderPasses, diagnosticMaxQueriesPerRenderPass, diagnosticPostRpBatches,
                 diagnosticResultCopies, diagnosticTimestampCopies, diagnosticBufferAttachments,
                 diagnosticBufferReuses, diagnosticPoolOverflowAttempts);
        }

        diagnosticReports = 0;
        diagnosticBegins = 0;
        diagnosticEnds = 0;
        diagnosticResets = 0;
        diagnosticPostRpBatches = 0;
        diagnosticResultCopies = 0;
        diagnosticTimestampCopies = 0;
        diagnosticBufferAttachments = 0;
        diagnosticBufferReuses = 0;
        diagnosticPoolOverflowAttempts = 0;
        diagnosticRenderPasses = 0;
        diagnosticMaxQueriesPerRenderPass = 0;
    }

    Queries::Queries(GPU &gpu) : counters{{{gpu.vkDevice, vk::QueryType::eOcclusion}}} {}

    void Queries::Query(InterconnectContext &ctx, soc::gm20b::IOVA address, CounterType type, std::optional<u64> timestamp) {
        view.Update(ctx, address, timestamp ? 16 : 4);
        usedQueryAddresses.emplace(u64{address});
        auto &counter{counters[static_cast<u32>(type)]};
        counter.RecordBufferAttachment(ctx.executor.AttachBuffer(*view));

        view->GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);
        counter.Report(ctx, *view, timestamp);
        counter.Begin(ctx);
    }

    void Queries::ResetCounter(InterconnectContext &ctx, CounterType type) {
        auto &counter{counters[static_cast<u32>(type)]};
        counter.RecordReset();
        counter.End(ctx);
        counter.Begin(ctx);
    }

    void Queries::PurgeCaches(InterconnectContext &ctx) {
        view.PurgeCaches();
        for (u32 i{}; i < static_cast<u32>(CounterType::MaxValue); i++) {
            counters[i].End(ctx);
            counters[i].LogAndResetDiagnostics(ctx.executor.submissionNumber);
        }
    }

    bool Queries::QueryPresentAtAddress(soc::gm20b::IOVA address) {
        return usedQueryAddresses.contains(u64{address});
    }
}
