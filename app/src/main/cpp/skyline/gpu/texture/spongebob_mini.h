// SPDX-License-Identifier: MPL-2.0
#pragma once

// Explicit, bounded Vulkan image snapshots for title-screen investigation.
// Does not acquire GPU locks, discard guest bytes, or change resource authority.
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <utility>
#include <vector>

namespace skyline::gpu::texture::diagnostic {
    struct SpongeBobMiniBinding {
        uint32_t ticIndex{UINT32_MAX}; // Unknown if no matching TIC-cache entry.
        uint64_t iova{};
        uint32_t matchingEntries{};
    };

    struct SpongeBobMiniDrawInfo {
        uint64_t pipelineHash{}, channelSequence{};
        std::array<uint64_t, 6> shaderHashes{}; // Maxwell 3D pipeline stage order.
        uint32_t kind{}, topology{}, indexed{}, first{}, count{}, instances{};
        std::vector<SpongeBobMiniBinding> sampledBindings;
    };

    struct SpongeBobMiniInfo {
        uint64_t operation{};
        uint32_t role{}; // 1 target-before, 2 target-after, 3..6 sampled inputs
        uint64_t image{}, generation{}, writeSerial{};
        uint32_t hostFormat{}, guestFormat{};
        uint32_t width{}, height{}, x{}, y{}, regionWidth{}, regionHeight{};
        uint32_t mip{}, layer{}, blockWidth{}, blockHeight{}, bytesPerBlock{}, size{};
        uint64_t pipelineHash{}, channelSequence{};
        std::array<uint64_t, 6> shaderHashes{};
        uint32_t drawKind{}, topology{}, indexed{}, first{}, count{}, instances{};
        uint32_t sampledOrdinal{UINT32_MAX}, ticIndex{UINT32_MAX}, matchingTics{};
        uint64_t ticIova{};
    };

    class SpongeBobMini {
        struct Entry {
            SpongeBobMiniInfo info;
            std::function<std::vector<uint8_t>()> read;
        };
        mutable std::mutex mutex;
        std::atomic<bool> armed{};
        bool frozen{};
        uint64_t operations{};
        std::vector<Entry> entries;
        static constexpr uint64_t MaxOperations{3};
        static constexpr size_t MaxEntries{MaxOperations * 6};

      public:
        bool Armed() const { return armed.load(std::memory_order_relaxed); }

        bool Arm() {
            std::lock_guard lock{mutex};
            if (frozen || armed || operations >= MaxOperations) return false;
            armed.store(true, std::memory_order_release);
            return true;
        }

        uint64_t Begin() {
            if (!Armed()) return 0;
            std::lock_guard lock{mutex};
            if (frozen || !armed || operations >= MaxOperations) return 0;
            armed.store(false, std::memory_order_release);
            return ++operations;
        }

        bool Add(SpongeBobMiniInfo info, std::function<std::vector<uint8_t>()> read) {
            std::lock_guard lock{mutex};
            if (frozen || !info.operation || info.operation > operations ||
                info.role < 1 || info.role > 6 || !info.size || entries.size() >= MaxEntries)
                return false;
            for (const auto &entry : entries)
                if (entry.info.operation == info.operation && entry.info.role == info.role)
                    return false;
            entries.push_back({info, std::move(read)});
            return true;
        }

        bool Export(std::ostream &out) {
            std::vector<Entry> copy;
            {
                std::lock_guard lock{mutex};
                // Verify all record pairs before freezing. The Android export
                // action may be pressed while the last command is still queued.
                if (armed || entries.empty()) return false;
                for (uint64_t op{1}; op <= operations; ++op) {
                    const Entry *before{}, *after{};
                    unsigned inputs{};
                    for (const auto &entry : entries) {
                        if (entry.info.operation != op) continue;
                        if (entry.info.role == 1) before = &entry;
                        if (entry.info.role == 2) after = &entry;
                        if (entry.info.role >= 3) ++inputs;
                    }
                    if (!before || !after || !inputs ||
                        before->info.image != after->info.image ||
                        before->info.generation != after->info.generation ||
                        before->info.size != after->info.size)
                        return false;
                }
                frozen = true;
                copy = entries;
            }

            // No resource or controller lock is held across GPU fence waits.
            std::vector<std::vector<uint8_t>> payloads;
            payloads.reserve(copy.size());
            for (const auto &entry : copy) {
                auto bytes{entry.read()};
                if (bytes.size() != entry.info.size) return false;
                payloads.emplace_back(std::move(bytes));
            }
            out << "SBMINI2\n";
            for (size_t i{}; i < copy.size(); ++i) {
                const auto &m{copy[i].info};
                out << m.operation << ',' << m.role << ',' << m.image << ','
                    << m.generation << ',' << m.writeSerial << ',' << m.hostFormat << ','
                    << m.guestFormat << ',' << m.width << ',' << m.height << ','
                    << m.x << ',' << m.y << ',' << m.regionWidth << ',' << m.regionHeight
                    << ',' << m.mip << ',' << m.layer << ',' << m.blockWidth << ','
                    << m.blockHeight << ',' << m.bytesPerBlock << ',' << m.size
                    << ',' << m.pipelineHash << ',' << m.channelSequence;
                for (auto shaderHash : m.shaderHashes) out << ',' << shaderHash;
                out << ',' << m.drawKind << ',' << m.topology << ',' << m.indexed
                    << ',' << m.first << ',' << m.count << ',' << m.instances
                    << ',' << m.sampledOrdinal << ',' << m.ticIndex
                    << ',' << m.matchingTics << ',' << m.ticIova << '\n';
                out.write(reinterpret_cast<const char *>(payloads[i].data()), payloads[i].size());
                out.put('\n');
            }
            return out.good();
        }
    };
}
