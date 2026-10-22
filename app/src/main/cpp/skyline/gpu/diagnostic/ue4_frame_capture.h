// SPDX-License-Identifier: MPL-2.0
#pragma once
// One-shot diagnostic only. This is a small, bounded trace, not the RenderDoc API.
// No guest data or guest GPU command is modified.
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <atomic>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace skyline::gpu::diagnostic {
    class Ue4FrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{12000};
        static constexpr std::uint32_t FrameCount{6};
        // Reserve budget for late UI composition and each of the four intervals.
        static constexpr std::uint32_t PassSnapshotsPerInterval{6};
        static constexpr std::uint32_t MaxPassSnapshots{30};
        static constexpr std::size_t MaxPassCaptureBytes{160ULL * 1024 * 1024};

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        // Phase 3 uses separate quotas from legacy pass-end snapshots.
        // A fixed-size, low-impact scan samples selected HDR draws and
        // the observed HDR->BGRA8 conversion instead of 30 unrelated inputs.
        // First actual HDR-producing interval can be later than presentation
        // 0/1 because guest rendering is asynchronous to Android presentation.
        std::optional<std::uint32_t> firstHdrInterval{};
        std::array<std::uint32_t, FrameCount> hdrDrawCountByInterval{};
        std::array<bool, FrameCount> conversionClaimed{};
        std::atomic<bool> hdrFirstBadEnabled{false};
        std::uintptr_t firstHdrTarget{};
        static constexpr std::uint32_t MaxHdrScanDraws{32};
        struct HdrScanStats {
            std::uint32_t ordinal{}, width{}, height{};
            std::size_t nan{}, infinity{}, over1000{}, over10000{};
            float maxFinite{};
            std::vector<std::uint64_t> badMask, highMask;
        };
        std::map<std::uint32_t, HdrScanStats> hdrScanResults;
        std::uint32_t focusSnapshotCount{};
        std::size_t focusBytes{};
        bool materialInputsClaimed{};
        struct SavedTargetShader {
            std::uint64_t hash{};
            bool spirv{};
            std::vector<std::uint8_t> bytes;
        };
        std::vector<SavedTargetShader> savedTargetShaders;

        static void WriteSavedTargetShader(const std::string &directory, const SavedTargetShader &entry) {
            std::ostringstream name;
            name << "shader_" << std::uppercase << std::hex << std::setw(16)
                 << std::setfill('0') << entry.hash
                 << (entry.spirv ? ".spv" : ".maxwell.bin");
            std::ofstream out(std::filesystem::path(directory) / name.str(), std::ios::binary | std::ios::trunc);
            if (out)
                out.write(reinterpret_cast<const char *>(entry.bytes.data()),
                          static_cast<std::streamsize>(entry.bytes.size()));
        }

        static constexpr std::uint32_t MaxFocusSnapshots{64};
        static constexpr std::size_t MaxFocusBytes{144ULL * 1024 * 1024};
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        // Guest backing ranges shared across Maxwell3D, Fermi2D and VI executors.
        // No texture pointers, shader hashes or game IDs are used as persistent keys.
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;

      public:
        bool HdrFirstBadMode() const {
            return hdrFirstBadEnabled.load(std::memory_order_acquire);
        }

        // R11G11B10 UINT-packed GPU readback, processed only after completion.
        // Threshold exceedance alone never establishes a rendering bug.
        void RecordHdrScan(std::uint32_t ordinal, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t *data, std::size_t bytes) {
            if (!HdrFirstBadMode() || !data || !width || !height ||
                bytes != static_cast<std::size_t>(width) * height * 4 ||
                ordinal > MaxHdrScanDraws)
                return;
            const auto decode{[](std::uint32_t v, int mantissaBits) -> float {
                const std::uint32_t m{v & ((1u << mantissaBits) - 1)};
                const std::uint32_t e{(v >> mantissaBits) & 31u};
                if (e == 31u)
                    return m ? std::numeric_limits<float>::quiet_NaN()
                             : std::numeric_limits<float>::infinity();
                if (!e)
                    return std::ldexp(static_cast<float>(m), -14 - mantissaBits);
                return std::ldexp(static_cast<float>((1u << mantissaBits) | m),
                                  static_cast<int>(e) - 15 - mantissaBits);
            }};
            HdrScanStats result;
            result.ordinal = ordinal;
            result.width = width;
            result.height = height;
            const std::size_t pixels{bytes / 4};
            result.badMask.resize((pixels + 63) / 64);
            result.highMask.resize((pixels + 63) / 64);
            for (std::size_t pixel{}; pixel < pixels; ++pixel) {
                const std::size_t off{pixel * 4};
                const std::uint32_t raw{static_cast<std::uint32_t>(data[off]) |
                    (static_cast<std::uint32_t>(data[off + 1]) << 8) |
                    (static_cast<std::uint32_t>(data[off + 2]) << 16) |
                    (static_cast<std::uint32_t>(data[off + 3]) << 24)};
                const std::array<float, 3> rgb{
                    decode(raw & 0x7ffu, 6), decode((raw >> 11) & 0x7ffu, 6),
                    decode((raw >> 22) & 0x3ffu, 5)};
                bool nan{}, inf{}, high{}, veryHigh{};
                for (float value : rgb) {
                    nan |= std::isnan(value);
                    inf |= std::isinf(value);
                    high |= value > 1000.f;
                    veryHigh |= value > 10000.f;
                    if (std::isfinite(value) && value > result.maxFinite)
                        result.maxFinite = value;
                }
                result.nan += nan;
                result.infinity += inf;
                result.over1000 += high;
                result.over10000 += veryHigh;
                if (nan || inf)
                    result.badMask[pixel >> 6] |= (1ULL << (pixel & 63));
                if (high)
                    result.highMask[pixel >> 6] |= (1ULL << (pixel & 63));
            }
            std::scoped_lock lock(mutex);
            hdrScanResults[ordinal] = std::move(result);
            const auto folder{std::filesystem::path(path)};
            std::ofstream csv(folder / "hdr_scan_stats.csv", std::ios::trunc);
            if (!csv)
                return;
            csv << "ordinal,phase,pixels,nan,inf,over1000,over10000,new_nonfinite,new_over1000,max_finite,previous_available\n";
            std::optional<std::uint32_t> firstNonfinite, firstHigh;
            std::uint32_t previousOrdinal{};
            const HdrScanStats *previous{};
            for (const auto &[id, current] : hdrScanResults) {
                std::size_t newBad{}, newHigh{};
                const bool adjacent{previous && previousOrdinal + 1 == id &&
                                    previous->width == current.width &&
                                    previous->height == current.height};
                if (adjacent) {
                    for (std::size_t i{}; i < current.badMask.size(); ++i) {
                        newBad += std::popcount(current.badMask[i] & ~previous->badMask[i]);
                        newHigh += std::popcount(current.highMask[i] & ~previous->highMask[i]);
                    }
                    if (newBad && !firstNonfinite)
                        firstNonfinite = id;
                    if (newHigh && !firstHigh)
                        firstHigh = id;
                }
                csv << id << ',' << (id ? "after_draw" : "before_draw_1") << ','
                    << static_cast<std::size_t>(current.width) * current.height << ','
                    << current.nan << ',' << current.infinity << ','
                    << current.over1000 << ',' << current.over10000 << ','
                    << newBad << ',' << newHigh << ',' << current.maxFinite << ','
                    << adjacent << '\n';
                previous = &current;
                previousOrdinal = id;
            }
            csv.close();
            std::ofstream summary(folder / "hdr_first_suspect.txt", std::ios::trunc);
            if (summary) {
                summary << "R11G11B10 HDR first-anomaly scan, sorted by draw ordinal.\n"
                        << "baseline_present=" << hdrScanResults.contains(0) << '\n'
                        << "snapshots_complete=" << (hdrScanResults.size() == MaxHdrScanDraws + 1) << '\n'
                        << "first_new_nonfinite_draw="
                        << (firstNonfinite ? std::to_string(*firstNonfinite) : "none_or_incomplete") << '\n'
                        << "first_new_over1000_draw="
                        << (firstHigh ? std::to_string(*firstHigh) : "none_or_incomplete") << '\n'
                        << "Nonfinite bit patterns are definite; large HDR values may be valid.\n"
                        << "A reported transition is a candidate, not a proved root cause.\n"
                        << "Gaps, intervening clears/copies and other GPU writers can confound attribution.\n";
            }
        }

        void Start(std::string captureDirectory) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            events.clear();
            const auto folder{std::filesystem::path(path)};
            // Control markers are placed in gpu_capture/ue4, not capture_001.
            // Start() receives the new session directory, so inspect its parent.
            const auto controlFolder{folder.parent_path()};
            const bool scanRequested{std::filesystem::exists(controlFolder / "ENABLE_HDR_FIRST_BAD")};
            const bool provenance{std::filesystem::exists(controlFolder / "ENABLE_UPLOAD_PROVENANCE")};
            hdrFirstBadEnabled.store(scanRequested && !provenance, std::memory_order_release);
            if (scanRequested && provenance)
                events.emplace_back("HDR_SCAN_DISABLED reason=conflicts_with_upload_provenance");
            else if (scanRequested)
                events.emplace_back("HDR_SCAN_ARMED mode=first_32_draws baseline=before_draw_1");
            hdrScanResults.clear();
            firstHdrTarget = 0;
            passCountByInterval.fill(0);
            firstHdrInterval.reset();
            hdrDrawCountByInterval.fill(0);
            conversionClaimed.fill(false);
            focusSnapshotCount = 0;
            focusBytes = 0;
            materialInputsClaimed = false;
            for (const auto &entry : savedTargetShaders)
                WriteSavedTargetShader(path, entry);
            nextPassIndex = 0;
            passBytes = 0;
            watchedMappings.clear();
            eventCount.store(static_cast<std::uint32_t>(events.size()), std::memory_order_relaxed);
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        // Bounded code artifacts for the observed HDR fragment shaders and
        // their matching vertex stages (original draw 20, first HDR peak at
        // draw 1, first green-channel NaN at draw 7). Shaders are often
        // compiled before ARM_CAPTURE, so retain only these identities.
        void RememberTargetShader(std::uint64_t hash, bool spirv,
                                  const std::uint8_t *bytes, std::size_t size) {
            if ((hash != 0x7161B14045F710B5ULL && hash != 0x027CCF34AF69591BULL &&
                 hash != 0xEA18B70958415FF3ULL && hash != 0xACF76CF575A996D5ULL &&
                 hash != 0x1B8CB95A440E18C1ULL && hash != 0x35F3C8B5DDB72D36ULL) ||
                !bytes || !size || size > 2ULL * 1024 * 1024)
                return;
            std::scoped_lock lock(mutex);
            for (const auto &existing : savedTargetShaders)
                if (existing.hash == hash && existing.spirv == spirv)
                    return;
            auto &entry{savedTargetShaders.emplace_back()};
            entry.hash = hash;
            entry.spirv = spirv;
            entry.bytes.assign(bytes, bytes + size);
            if (active.load(std::memory_order_relaxed) && !path.empty())
                WriteSavedTargetShader(path, entry);
        }

        // Exactly one complete material-resource capture on an actual
        // suspect draw. The raw snapshots share the global byte quota.
        bool ClaimMaterialInputs() {
            if (HdrFirstBadMode() || !active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || materialInputsClaimed)
                return false;
            materialInputsClaimed = true;
            return true;
        }

        // One-shot phase 3. All indexes and claims are shared by executor threads.
        // Returns the draw ordinal only at sampling points; the rest do not
        // cause staging allocations or render-pass breaks.
        std::optional<std::uint32_t> ClaimHdrDraw(std::uintptr_t targetIdentity = 0) {
            if (!active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            const auto frame{presentIndex.load(std::memory_order_relaxed)};
            if (!active.load(std::memory_order_relaxed) || frame >= FrameCount)
                return std::nullopt;
            // Previous capture: no HDR draws until interval 2. Do not spend
            // both HDR probes on fixed presentation indices 0 and 1.
            if (!firstHdrInterval)
                firstHdrInterval = frame;
            if (HdrFirstBadMode()) {
                if (frame != *firstHdrInterval)
                    return std::nullopt;
                if (!targetIdentity)
                    return std::nullopt;
                if (!firstHdrTarget)
                    firstHdrTarget = targetIdentity;
                if (firstHdrTarget != targetIdentity)
                    return std::nullopt;
                const auto ordinal{++hdrDrawCountByInterval[frame]};
                return ordinal <= MaxHdrScanDraws ? std::optional<std::uint32_t>(ordinal)
                                                 : std::nullopt;
            }
            if (frame < *firstHdrInterval || frame - *firstHdrInterval >= 2)
                return std::nullopt;
            const auto ordinal{++hdrDrawCountByInterval[frame]};
            // The latest pair analysis localized the greatest HDR amplification
            // to draws 19–21. Concentrate the expensive readbacks here.
            if (ordinal == 19 || ordinal == 20 || ordinal == 21)
                return ordinal;
            return std::nullopt;
        }

        bool ClaimConversion() {
            if (HdrFirstBadMode() || !active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            const auto frame{presentIndex.load(std::memory_order_relaxed)};
            if (!active.load(std::memory_order_relaxed) || frame >= FrameCount ||
                conversionClaimed[frame])
                return false;
            conversionClaimed[frame] = true;
            return true;
        }

        std::optional<std::uint32_t> ReserveFocusedImage(std::size_t bytes) {
            if (!bytes || !active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) ||
                focusSnapshotCount >= MaxFocusSnapshots ||
                bytes > MaxFocusBytes || focusBytes > MaxFocusBytes - bytes)
                return std::nullopt;
            focusBytes += bytes;
            return focusSnapshotCount++;
        }

        // Allocates a unique, globally capped snapshot ID across all executors.
        // Called before allocating staging buffers or writing file names.
        std::optional<std::uint32_t> ReservePassSnapshot(std::uint32_t interval, std::size_t bytes) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) ||
                interval >= FrameCount || bytes == 0 ||
                passCountByInterval[interval] >= PassSnapshotsPerInterval ||
                nextPassIndex >= MaxPassSnapshots ||
                passBytes > MaxPassCaptureBytes ||
                bytes > MaxPassCaptureBytes - passBytes)
                return std::nullopt;

            ++passCountByInterval[interval];
            passBytes += bytes;
            return nextPassIndex++;
        }

        // One-shot diagnostic: a blended draw sampling compressed assets may
        // produce a sprite/UI layer. Follow its backing across executors.
        void WatchMapping(std::uintptr_t address, std::size_t bytes) {
            if (!address || !bytes || !active.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed))
                return;
            for (const auto &entry : watchedMappings)
                if (entry.first == address && entry.second == bytes)
                    return;
            if (watchedMappings.size() < 32)
                watchedMappings.emplace_back(address, bytes);
        }

        bool IsWatchedMapping(std::uintptr_t address, std::size_t bytes) {
            if (!address || !bytes || !active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            for (const auto &[start, length] : watchedMappings) {
                if (address >= start ? address - start < length : start - address < bytes)
                    return true;
            }
            return false;
        }

        std::string CaptureDirectory() {
            std::scoped_lock lock(mutex);
            return path;
        }

        void Record(std::string event) {
            if (!active.load(std::memory_order_acquire) ||
                eventCount.load(std::memory_order_relaxed) >= MaxEvents)
                return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || events.size() >= MaxEvents)
                return;
            events.emplace_back("present_index_approx=" +
                std::to_string(presentIndex.load(std::memory_order_relaxed)) + " " +
                std::move(event));
            eventCount.store(static_cast<std::uint32_t>(events.size()), std::memory_order_relaxed);
        }

        void Finish() {
            active.store(false, std::memory_order_release);
            std::scoped_lock lock(mutex);
            std::ofstream out(std::filesystem::path(path) / "gpu_events.txt", std::ios::trunc);
            if (out) {
                out << "GPU events collected while 6 frames were being presented.\n"
                    << "present_index_approx is NOT exact GPU/present synchronization.\n"
                    << "Events are capped at 12000 to bound memory and disk usage.\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
        }
    };

    inline Ue4FrameCapture &Ue4Capture() {
        static Ue4FrameCapture session;
        return session;
    }
}