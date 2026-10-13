// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "re5_scalar_probe.h"

namespace skyline::gpu::diagnostic {
    inline std::uint64_t NextDiagnosticIdentity() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    struct MemoryRange { std::uint64_t address{}, size{}; };
    inline bool RangesOverlap(MemoryRange a, MemoryRange b) {
        // Reject wrapped/unverifiable spans instead of associating unrelated resources.
        if (!a.size || !b.size || a.size > UINT64_MAX - a.address || b.size > UINT64_MAX - b.address)
            return false;
        return a.address < b.address + b.size && b.address < a.address + a.size;
    }
    struct BlitRecord {
        std::uint64_t id{}, texture{};
        std::string text;
        std::vector<MemoryRange> ranges;
        std::uint64_t sourceTexture{};
    };

    // Bounded latest-producer cache, not a complete GPU execution history.
    // Repeated screen/UI blits replace one destination's entry instead of
    // evicting every texture initialized during chapter loading.
    class BlitJournal {
      public:
        static constexpr std::size_t MaxTextures{256}, MaxPinned{128}, MaxRanges{16};
        static constexpr std::size_t MaxRecordBytes{16 * 1024}, MaxSnapshotBytes{8 * 1024 * 1024};
      private:
        mutable std::mutex mutex;
        std::map<std::uint64_t, BlitRecord> latest;
        std::map<std::uint64_t, BlitRecord> pinned;
        struct Sync { std::uint64_t sequence{}, uploads{}, downloads{}, guestReturns{}; std::string text; };
        std::map<std::uint64_t, Sync> sync;
        std::map<std::uint64_t, std::string> recordedBlits;
        std::uint64_t evictions{}, syncSequence{}, pinDrops{};
        std::string capturePath;
        bool captureOpen{};
      public:
        void Store(BlitRecord record) {
            std::scoped_lock lock(mutex);
            if (record.text.size() > MaxRecordBytes)
                record.text.resize(MaxRecordBytes);
            if (record.ranges.size() > MaxRanges)
                record.ranges.resize(MaxRanges);
            const auto key{record.texture ? record.texture : (record.id | (1ULL << 63))};
            if (!latest.contains(key) && latest.size() == MaxTextures) {
                const auto oldest{std::min_element(latest.begin(), latest.end(), [](const auto &a, const auto &b) {
                    return a.second.id < b.second.id;
                })};
                latest.erase(oldest);
                ++evictions;
            }
            for (const auto identity : {record.texture, record.sourceTexture}) {
                if (!identity) continue;
                if (!sync.contains(identity) && sync.size() == MaxTextures * 2)
                    sync.erase(sync.begin());
                sync.try_emplace(identity);
            }
            latest[key] = std::move(record);
        }
        void BeginCapture(std::string path) {
            std::scoped_lock lock(mutex);
            capturePath = std::move(path);
            captureOpen = true;
            pinned.clear();
            pinDrops = 0;
        }
        std::vector<std::uint64_t> PinRelated(const std::string &path, std::uint64_t texture,
                                             const std::vector<MemoryRange> &ranges) {
            std::scoped_lock lock(mutex);
            std::vector<std::uint64_t> result;
            if (path != capturePath || !captureOpen) return result;
            for (const auto &[key, record] : latest) {
                bool related{record.texture == texture};
                if (!related)
                    for (const auto &a : record.ranges)
                        for (const auto &b : ranges)
                            related |= RangesOverlap(a, b);
                if (!related) continue;
                if (!pinned.contains(record.id) && pinned.size() >= MaxPinned) {
                    ++pinDrops;
                    continue;
                }
                pinned[record.id] = record;
                result.push_back(record.id);
            }
            return result;
        }
        bool Tracks(std::uint64_t texture) const {
            std::scoped_lock lock(mutex);
            return sync.contains(texture);
        }
        void NoteSync(std::uint64_t texture, std::string text) {
            std::scoped_lock lock(mutex);
            if (auto it{sync.find(texture)}; it != sync.end()) {
                auto &state{it->second};
                state.sequence = ++syncSequence;
                state.uploads += text.starts_with("upload_copy_recorded");
                state.downloads += text.starts_with("download_copy_recorded");
                state.guestReturns += text.starts_with("copy_to_guest_returned");
                state.text = text.substr(0, 512);
            }
        }
        void NoteBlitRecorded(std::uint64_t id, std::string text) {
            if (!id) return;
            std::scoped_lock lock(mutex);
            if (!recordedBlits.contains(id) && recordedBlits.size() == MaxTextures * 2)
                recordedBlits.erase(recordedBlits.begin());
            recordedBlits[id] = text.substr(0, 1024);
        }
      private:
        std::string SnapshotLocked(const std::string &path) const {
            std::string result{"BLIT_HISTORY coverage=latest_destination_cache_not_complete_execution_trace evictions="};
            result += std::to_string(evictions) + " pin_drops=" + std::to_string(pinDrops) + "\n";
            if (path != capturePath) return result + "capture_path_mismatch\n";
            auto records{pinned};
            for (const auto &[key, record] : latest) records.try_emplace(record.id, record);
            for (const auto &[id, record] : records) {
                const auto block{"\nBLIT id=" + std::to_string(id) + " destination_texture_id=" +
                    std::to_string(record.texture) + " source_texture_id=" + std::to_string(record.sourceTexture) +
                    "\n" + record.text + "\nEND_BLIT\n"};
                if (block.size() > MaxSnapshotBytes - result.size())
                    return result + "snapshot_truncated\n";
                result += block;
                if (auto it{recordedBlits.find(id)}; it != recordedBlits.end())
                    result += "BLIT_CMD_RECORDED id=" + std::to_string(id) + " " + it->second + "\n";
            }
            for (const auto &[id, state] : sync)
                result += "SYNC_LAST texture_id=" + std::to_string(id) + " sequence=" +
                    std::to_string(state.sequence) + " uploads=" + std::to_string(state.uploads) +
                    " downloads=" + std::to_string(state.downloads) + " guest_copy_returns=" +
                    std::to_string(state.guestReturns) + " coverage=since_tracking_enrollment " + state.text + "\n";
            return result;
        }
      public:
        std::string Snapshot(const std::string &path) const {
            std::scoped_lock lock(mutex);
            return SnapshotLocked(path);
        }
        std::string FreezeSnapshot(const std::string &path) {
            std::scoped_lock lock(mutex);
            if (path == capturePath) captureOpen = false;
            return SnapshotLocked(path);
        }
        void TrackTexture(std::uint64_t id) {
            if (!id) return;
            std::scoped_lock lock(mutex);
            if (!sync.contains(id) && sync.size() == MaxTextures * 2)
                sync.erase(sync.begin());
            sync.try_emplace(id);
        }
        std::size_t Size() const { std::scoped_lock lock(mutex); return latest.size(); }
    };
    inline BlitJournal &Re5BlitJournal() { static BlitJournal journal; return journal; }

    class BlitScope;
    inline thread_local BlitScope *currentBlit{};
    inline BlitScope *CurrentBlit() { return currentBlit; }
    class BlitScope {
        BlitScope *previous{currentBlit};
        int exceptions{std::uncaught_exceptions()};
        std::string text;
        bool truncated{};
      public:
        const std::uint64_t id{NextDiagnosticIdentity()};
        const char *role{"operation"};
        std::uint64_t sourceTexture{}, destinationTexture{};
        std::vector<MemoryRange> destinationRanges;
        bool queued{};
        BlitScope() { currentBlit = this; }
        void Append(std::string event) {
            if (text.size() >= BlitJournal::MaxRecordBytes) { truncated = true; return; }
            event = "role=" + std::string(role) + " " + std::move(event) + "\n";
            const auto available{BlitJournal::MaxRecordBytes - text.size()};
            truncated |= event.size() > available;
            text += event.substr(0, available);
        }
        ~BlitScope() {
            currentBlit = previous;
            try {
                Append(std::string("preparation_status=") + (std::uncaught_exceptions() > exceptions ? "exception" : queued ? "queued_not_gpu_completion" : "incomplete"));
                if (truncated) {
                    const std::string marker{"\nrecord_text_truncated=1\n"};
                    text.resize(BlitJournal::MaxRecordBytes - marker.size());
                    text += marker;
                }
                Re5BlitJournal().Store({id, destinationTexture, std::move(text), std::move(destinationRanges), sourceTexture});
            } catch (...) { /* Diagnostics must not replace the original exception. */ }
        }
    };

    template<typename Slots, typename Writes, typename Copies>
    void UpdateImageProvenance(Slots &current, const Writes &writes, const Copies &copies, std::size_t limit) {
        std::erase_if(current, [&](const auto &slot) {
            return !std::any_of(copies.begin(), copies.end(), [&](const auto &copy) {
                return IdentityCopyCovers(slot.binding, slot.element, copy.srcBinding, copy.srcArrayElement,
                    copy.dstBinding, copy.dstArrayElement, copy.descriptorCount);
            });
        });
        for (const auto &write : writes) {
            const auto it{std::find_if(current.begin(), current.end(), [&](const auto &slot) {
                return slot.binding == write.binding && slot.element == write.element;
            })};
            if (it != current.end()) *it = write;
            else if (current.size() < limit) current.push_back(write);
        }
    }
}
