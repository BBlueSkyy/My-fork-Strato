// SPDX-License-Identifier: MPL-2.0
// Temporary consumer-boundary diagnostic; do not merge the hooks into master.
#include <cstring>
#include <input.h>
#include <input/npad_sdk_probe.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>
#include "npad_sdk_probe.h"
#include "probe_read.h"

namespace skyline::hle {
    namespace {
        using input::diagnostic::NpadOutput;
        using input::diagnostic::NpadReader;

        struct Call {
            NpadReader reader;
            u64 output;
            i32 requested;
            u32 id{};
            bool valid{};
            std::array<NpadOutput, 17> before{};
            std::array<u64, 7> tails{}, counts{};
        };
        struct Seen {
            const DeviceState *state;
            size_t layout;
            bool plural;
            u32 id;
            u64 calls{};
            NpadOutput previous{};
        };
        thread_local std::vector<Call> pending;
        thread_local std::vector<Seen> seen;

        // Read host mirrors without writing output buffers or the CPU context.
        // Reject unknown/non-readable ranges instead of installing memory traps.
        bool ReadGuest(const DeviceState &state, u64 address, void *destination, size_t size) {
            auto &memory{state.process->memory};
            const auto start{reinterpret_cast<u64>(memory.addressSpace.data())};
            if (address < start || address - start > memory.addressSpace.size() ||
                size > memory.addressSpace.size() - (address - start))
                return false;
            span<u8> range{reinterpret_cast<u8 *>(address), size};
            if (!memory.IsRangeMapped(range))
                return false;
            for (auto cursor{address}; cursor < address + size;) {
                auto chunk{memory.GetChunk(reinterpret_cast<u8 *>(cursor))};
                if (!chunk || !chunk->second.permission.r)
                    return false;
                const auto end{reinterpret_cast<u64>(chunk->first) + chunk->second.size};
                if (end <= cursor)
                    return false;
                cursor = std::min<u64>(end, address + size);
            }
            const auto host{memory.GetHostSpan(range)};
            // Mapping validation above is advisory: another guest thread may
            // unmap/reprotect the buffer immediately afterwards. A kernel copy
            // returns EFAULT/short-read rather than faulting this diagnostic.
            // A private nonblocking pipe is the fault-safe fallback. Failure
            // reports no-output; never use an unsafe userspace dereference.
            return ReadProbeMemory(destination, host.data(), size);
        }

        std::array<const input::NpadControllerInfo *, 7> Rings(const input::NpadSection &section) {
            return {&section.fullKeyController, &section.handheldController,
                    &section.dualController, &section.leftController,
                    &section.rightController, &section.palmaController,
                    &section.defaultController};
        }

        void Enter(const DeviceState &state, NpadReader reader) {
            const auto &ctx{static_cast<kernel::type::KNceThread &>(*state.thread).ctx};
            Call call{.reader = reader, .output = ctx.gpr.x0,
                .requested = reader.plural ? static_cast<i32>(ctx.gpr.w1) : 1};
            const auto idAddress{reader.plural ? ctx.gpr.x2 : ctx.gpr.x1};
            call.valid = state.input && call.requested > 0 && call.requested <= 17 &&
                ReadGuest(state, idAddress, &call.id, sizeof(call.id)) &&
                input::diagnostic::NpadSlot(call.id).has_value() &&
                ReadGuest(state, call.output, call.before.data(), sizeof(NpadOutput) * call.requested);
            if (call.valid) {
                std::scoped_lock lock{state.input->npad.mutex};
                auto rings{Rings(state.input->hid->npad[*input::diagnostic::NpadSlot(call.id)])};
                for (size_t i{}; i < rings.size(); ++i) {
                    call.tails[i] = rings[i]->header.currentEntry;
                    call.counts[i] = rings[i]->header.maxEntry;
                }
            }
            pending.push_back(call);
        }

        void Exit(const DeviceState &state, const HookedSymbol &symbol, NpadReader reader) {
            if (pending.empty()) {
                LOGW("HID-SDK coverage-error missing-entry reader={}", symbol.prettyName);
                return;
            }
            auto call{pending.back()};
            pending.pop_back();
            if (call.reader.layout != reader.layout || call.reader.plural != reader.plural) {
                LOGW("HID-SDK coverage-error mismatched-entry reader={}", symbol.prettyName);
                return;
            }
            const auto &ctx{static_cast<kernel::type::KNceThread &>(*state.thread).ctx};
            std::array<NpadOutput, 17> after{};
            if (!call.valid || !ReadGuest(state, call.output, after.data(), sizeof(NpadOutput) * call.requested)) {
                LOGW("HID-SDK no-output reader={} requested={} out=0x{:X} rawX0=0x{:X}",
                     symbol.prettyName, call.requested, call.output, ctx.gpr.x0);
                return;
            }
            auto previous{std::find_if(seen.begin(), seen.end(), [&](const Seen &entry) {
                return entry.state == &state && entry.layout == reader.layout &&
                    entry.plural == reader.plural && entry.id == call.id;
            })};
            if (previous == seen.end()) {
                seen.push_back({&state, reader.layout, reader.plural, call.id});
                previous = std::prev(seen.end());
            }
            ++previous->calls;
            const auto &a{after[0]};
            const auto &b{previous->previous};
            // Keep the first eight calls, payload changes and every 128th call.
            // This is a logging budget, not a delay or a HID sampling change.
            const bool report{previous->calls <= 8 || previous->calls % 128 == 0 ||
                a.buttons != b.buttons || a.lx != b.lx || a.ly != b.ly ||
                a.rx != b.rx || a.ry != b.ry || a.attributes != b.attributes};
            previous->previous = a;
            if (!report)
                return;

            LOGI("HID-SDK returned reader={} id=0x{:X} call={} requested={} out=0x{:X} rawX0=0x{:X}",
                 symbol.prettyName, call.id, previous->calls, call.requested, call.output, ctx.gpr.x0);
            // The function's return type is absent from its mangled name. rawX0
            // is NOT interpreted as a count/result, and output slots are not
            // labelled accepted samples merely because they are readable.
            std::array<input::NpadControllerInfo, 7> producer{};
            u32 style{};
            {
                std::scoped_lock lock{state.input->npad.mutex};
                const auto &section{state.input->hid->npad[*input::diagnostic::NpadSlot(call.id)]};
                style = static_cast<u32>(section.header.type);
                auto rings{Rings(section)};
                for (size_t i{}; i < rings.size(); ++i)
                    producer[i] = *rings[i];
            }
            for (size_t i{}; i < producer.size(); ++i)
                LOGI("HID-SDK ring={} style=0x{:X} beforeTail={} beforeCount={} afterTail={} afterCount={}",
                     i, style, call.tails[i], call.counts[i], producer[i].header.currentEntry, producer[i].header.maxEntry);
            for (i32 i{}; i < call.requested; ++i) {
                const auto &output{after[i]};
                size_t matches{};
                for (size_t ring{}; ring < producer.size(); ++ring) {
                    for (size_t slot{}; slot < producer[ring].state.size(); ++slot) {
                        const auto &sample{producer[ring].state[slot]};
                        NpadOutput payload{};
                        std::memcpy(&payload, &sample.localTimestamp, sizeof(payload));
                        if (payload == output && sample.globalTimestamp == (output.samplingNumber << 1)) {
                            ++matches;
                            LOGI("HID-SDK correspondence outputSlot={} ring={} sampleSlot={} sample={} marker={}",
                                 i, ring, slot, output.samplingNumber, sample.globalTimestamp);
                        }
                    }
                }
                LOGI("HID-SDK outputSlot={} sample={} buttons=0x{:X} LX={} LY={} RX={} RY={} attr=0x{:X} reserved=0x{:X} unchanged={} producerMatches={}",
                     i, output.samplingNumber, output.buttons, output.lx, output.ly, output.rx, output.ry,
                     output.attributes, output.reserved, output == call.before[i], matches);
            }
        }
    }

    std::optional<HookType> MakeNpadSdkProbe(std::string_view prettyName) {
        auto reader{input::diagnostic::IdentifyReader(prettyName)};
        if (!reader)
            return std::nullopt;
        return EntryExitHook{
            .entry = [reader = *reader](const DeviceState &state, const HookedSymbol &) { Enter(state, reader); },
            .exit = [reader = *reader](const DeviceState &state, const HookedSymbol &symbol) { Exit(state, symbol, reader); },
        };
    }
}
