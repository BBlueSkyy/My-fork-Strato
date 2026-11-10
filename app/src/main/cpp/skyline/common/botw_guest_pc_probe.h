// SPDX-License-Identifier: MPL-2.0
// Temporary passive BOTW NCE guest-PC diagnostic. Reads the interrupted
// AArch64 PC from EXISTING scheduler yield/preemption signals only.
// No additional signals, ptrace, thread suspension, or stack unwinding.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
#include "botw_freeze_probe.h"

namespace skyline::diag {
    struct BotwGuestPcSlot {
        std::atomic<std::uint64_t> samples{};
        std::atomic<std::uint64_t> samePcSamples{};
        std::atomic<std::uint64_t> lastPc{};
        std::atomic<std::uint64_t> lastLr{};
        std::atomic<std::uint64_t> lastSp{};
    };

    inline std::array<BotwGuestPcSlot, MaxTrackedGuestThreads> guestPcSlots{};

    // Async-signal-safe on Android AArch64: only lock-free integral atomics.
    // Never log, allocate, inspect guest memory, or take a mutex here.
    inline void RecordBotwGuestPc(std::size_t id, std::uint64_t pc,
                                  std::uint64_t lr, std::uint64_t sp) noexcept {
        if (!BotwProbesEnabled() || id >= MaxTrackedGuestThreads)
            return;
        auto &slot{guestPcSlots[id]};
        const auto previous{slot.lastPc.exchange(pc, std::memory_order_relaxed)};
        slot.lastLr.store(lr, std::memory_order_relaxed);
        slot.lastSp.store(sp, std::memory_order_relaxed);
        if (previous == pc)
            slot.samePcSamples.fetch_add(1, std::memory_order_relaxed);
        slot.samples.fetch_add(1, std::memory_order_release);
    }

    class BotwGuestPcSampler {
        std::array<std::uint64_t, MaxTrackedGuestThreads> priorSamples{};
        std::array<std::uint64_t, MaxTrackedGuestThreads> priorSamePc{};

      public:
        std::string Snapshot() {
            struct ThreadDelta {
                std::size_t id;
                std::uint64_t samples;
                std::uint64_t repeats;
                std::uint64_t pc;
                std::uint64_t lr;
                std::uint64_t sp;
            };
            std::vector<ThreadDelta> active;
            active.reserve(MaxTrackedGuestThreads);
            std::uint64_t totalSamples{};
            std::size_t observedThreads{};

            for (std::size_t id{}; id < MaxTrackedGuestThreads; ++id) {
                const auto &slot{guestPcSlots[id]};
                // Independent atomic snapshots are approximate: registers may
                // come from different samples and repeats may cross intervals.
                const auto count{slot.samples.load(std::memory_order_acquire)};
                const auto same{slot.samePcSamples.load(std::memory_order_relaxed)};
                const auto delta{count >= priorSamples[id] ? count - priorSamples[id] : 0};
                const auto repeatDelta{same >= priorSamePc[id] ? same - priorSamePc[id] : 0};
                priorSamples[id] = count;
                priorSamePc[id] = same;

                if (count)
                    ++observedThreads;
                if (!delta)
                    continue;
                totalSamples += delta;
                active.push_back({
                    id, delta, repeatDelta,
                    slot.lastPc.load(std::memory_order_relaxed),
                    slot.lastLr.load(std::memory_order_relaxed),
                    slot.lastSp.load(std::memory_order_relaxed),
                });
            }

            std::sort(active.begin(), active.end(),
                [](const auto &a, const auto &b) {
                    return a.samples == b.samples ? a.id < b.id : a.samples > b.samples;
                });

            std::ostringstream out;
            out << "BOTW_GUEST_PC_PROBE: passiveGuestSignalSamples=" << totalSamples
                << " activeThreads=" << active.size()
                << " everObservedThreads=" << observedThreads
                << " (PC/LR/SP from scheduler yield/preemption signals, "
                   "approximate independent atomic snapshots; registers may mix samples "
                   "and samePc may cross report boundaries; "
                   "no extra interrupts; no samples != stopped thread)";
            const auto limit{std::min(active.size(), std::size_t{12})};
            for (std::size_t i{}; i < limit; ++i) {
                const auto &t{active[i]};
                out << " HOS-" << t.id
                    << "{samples=" << t.samples
                    << ",samePc=" << t.repeats
                    << ",pc=0x" << std::hex << t.pc
                    << ",lr=0x" << t.lr
                    << ",sp=0x" << t.sp << std::dec << "}";
            }
            if (active.size() > limit)
                out << " otherActiveThreads=" << (active.size() - limit);
            return out.str();
        }
    };
}
