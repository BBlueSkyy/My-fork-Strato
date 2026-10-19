// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstring>
#include <gpu/interconnect/command_executor.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>

namespace skyline::gpu::interconnect {
    // TIC/TSC metadata is CPU-authored state, distinct from GPU-authored handles.
    // Re-read mappings and content instead of trusting register dirtiness as a
    // content epoch. GPU/queued writes need coherent metadata support first.
    template<typename Header>
    void ReadDynamicPool(InterconnectContext &ctx, u64 address, size_t count,
                         std::vector<Header> &snapshot) {
        constexpr size_t MaximumTicCount{1U << 20};
        if (!count || count > MaximumTicCount || !address)
            throw exception("Invalid dynamic descriptor pool address/count");
        const size_t bytes{count * sizeof(Header)};
        const auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(address, bytes)};
        size_t mapped{};
        for (const auto &mapping : mappings) {
            if (!mapping.valid() || mapping.empty() || mapping.size() > bytes - mapped)
                throw exception("Unmapped dynamic descriptor pool metadata");
            const auto interval{typename IntervalList<u8 *>::Interval{mapping.data(), mapping.data() + mapping.size()}};
            if (ctx.executor.usageTracker.dirtyIntervals.Intersect(interval) ||
                ctx.executor.usageTracker.sequencedIntervals.Intersect(interval))
                throw exception("GPU/queued writes to dynamic TIC/TSC metadata require coherent pool synchronization");
            mapped += mapping.size();
        }
        if (mapped != bytes)
            throw exception("Incomplete dynamic descriptor pool mapping");
        snapshot.resize(count);
        auto *output{reinterpret_cast<u8 *>(snapshot.data())};
        for (const auto &mapping : mappings) {
            std::memcpy(output, mapping.data(), mapping.size());
            output += mapping.size();
        }
    }
}
