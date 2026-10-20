// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace skyline::gpu::diagnostic {
    /**
     * Manual, bounded diagnostic for the Cosmic Shake good->black render transition.
     * It never changes guest state or resource selection. The presentation
     * thread arms/stops it; GPU/Texman threads only record while active.
     */
    class CosmicShakePassCapture {
      public:
        static constexpr std::uint32_t FrameCount{6};
        static constexpr std::uint32_t MaxPasses{36};
        static constexpr std::uint32_t MaxTexmanEvents{512};
        static constexpr std::uint32_t MaxDistinctTicFaults{256};
        static constexpr std::size_t MaxReadbackBytes{96ULL * 1024 * 1024};
        // Separate low-footprint sampling budget, not charged as full-image captures.
        static constexpr std::uint32_t MaxFocusedProbes{64};
        static constexpr std::size_t MaxFocusedBytes{128ULL * 1024};

        struct PassTicket {
            std::uint32_t index{};
            std::uint32_t present{};
            std::string directory;
        };
        struct FocusedTicket {
            std::uint32_t index{};
            std::uint32_t present{};
            std::string directory;
        };

        std::atomic<bool> active{false};
        std::atomic<bool> monitoring{false}; //!< Metadata-only history until a visual trigger.
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint64_t> monitorPresent{0};

      private:
        std::mutex mutex;
        std::string path;
        std::uint32_t nextPass{};
        std::uint32_t texmanEvents{};
        std::uint64_t totalTicFaults{};
        std::unordered_set<std::uint64_t> observedTicIovas;
        std::size_t readbackBytes{};
        std::uint32_t focusedProbes{};
        std::size_t focusedBytes{};
        std::deque<std::string> recentTexman, recentPasses, recentProbes, recentPresentSources;

        static void PushRecent(std::deque<std::string> &ring, std::string line, std::size_t limit) {
            if (ring.size() >= limit)
                ring.pop_front();
            ring.push_back(std::move(line));
        }

        static void WriteRecent(const std::filesystem::path &directory, const char *name,
                                const std::deque<std::string> &ring) {
            std::ofstream out(directory / name, std::ios::trunc);
            for (const auto &line : ring)
                if (out)
                    out << line << '\n';
        }

        void AppendLocked(const char *name, const std::string &line) {
            if (path.empty())
                return;
            std::ofstream out(std::filesystem::path(path) / name, std::ios::app);
            if (out)
                out << line << '\n';
        }

      public:
        void EnableMonitoring() {
            std::scoped_lock lock{mutex};
            if (active.load(std::memory_order_acquire) || monitoring.load(std::memory_order_acquire))
                return;
            recentTexman.clear();
            recentPasses.clear();
            recentProbes.clear();
            recentPresentSources.clear();
            monitorPresent.store(0, std::memory_order_relaxed);
            monitoring.store(true, std::memory_order_release);
        }

        bool RecordingMetadata() const {
            return active.load(std::memory_order_acquire) || monitoring.load(std::memory_order_acquire);
        }

        void Start(std::string directory) {
            std::scoped_lock lock{mutex};
            monitoring.store(false, std::memory_order_release);
            path = std::move(directory);
            WriteRecent(path, "pretrigger_texman.txt", recentTexman);
            WriteRecent(path, "pretrigger_passes.txt", recentPasses);
            WriteRecent(path, "pretrigger_screen_probes.txt", recentProbes);
            WriteRecent(path, "pretrigger_present_sources.txt", recentPresentSources);
            recentPresentSources.clear();
            recentTexman.clear();
            recentPasses.clear();
            recentProbes.clear();
            nextPass = 0;
            texmanEvents = 0;
            totalTicFaults = 0;
            observedTicIovas.clear();
            readbackBytes = 0;
            focusedProbes = 0;
            focusedBytes = 0;
            presentIndex.store(0, std::memory_order_relaxed);
            {
                std::ofstream(std::filesystem::path(path) / "pass_boundary.txt", std::ios::trunc)
                    << "Cosmic Shake pass-boundary diagnostic; stats are post-render-pass observations.\n";
                std::ofstream(std::filesystem::path(path) / "texman_decisions.txt", std::ios::trunc)
                    << "Bounded TextureManager decisions while capture is active.\n";
                std::ofstream(std::filesystem::path(path) / "tic_gmmu_faults.txt", std::ios::trunc)
                    << "First " << MaxDistinctTicFaults << " distinct unmapped TIC IOVAs during capture. "
                       "Context is the descriptor lookup submission/pass, not a guaranteed draw ID.\n";
            }
            active.store(true, std::memory_order_release);
        }

        void Stop() {
            std::scoped_lock lock{mutex};
            if (!active.exchange(false, std::memory_order_acq_rel))
                return;
            AppendLocked("tic_gmmu_faults.txt",
                "SUMMARY attempted_unmapped_lookups=" + std::to_string(totalTicFaults) +
                " distinct_recorded_iovas=" + std::to_string(observedTicIovas.size()) +
                " distinct_limit=" + std::to_string(MaxDistinctTicFaults));
        }

        // The fast inactive test avoids lock/formatting overhead in normal emulation.
        // Record only the first failure per IOVA to avoid huge files when a guest
        // repeatedly samples the same unmapped texture thousands of times.
        bool ReserveTicFault(std::uint64_t iova) {
            if (!active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_relaxed))
                return false;
            ++totalTicFaults;
            if (observedTicIovas.size() >= MaxDistinctTicFaults)
                return false;
            return observedTicIovas.insert(iova).second;
        }

        void RecordTicFault(std::string line) {
            std::scoped_lock lock{mutex};
            if (!path.empty())
                AppendLocked("tic_gmmu_faults.txt", line);
        }

        std::optional<PassTicket> ReservePass(std::size_t bytes) {
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_acquire) || !bytes ||
                nextPass >= MaxPasses || bytes > MaxReadbackBytes - readbackBytes)
                return std::nullopt;
            readbackBytes += bytes;
            return PassTicket{nextPass++, presentIndex.load(std::memory_order_relaxed), path};
        }

        // Charge both GPU staging allocations atomically. A paired capture
        // must never silently record only PRE or POST, and must not overflow
        // the shared full-image budget.
        std::optional<PassTicket> ReservePairedPass(std::size_t backingBytes) {
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_acquire) || !backingBytes ||
                nextPass >= MaxPasses ||
                backingBytes > (MaxReadbackBytes - readbackBytes) / 2)
                return std::nullopt;
            readbackBytes += 2 * backingBytes;
            return PassTicket{nextPass++, presentIndex.load(std::memory_order_relaxed), path};
        }

        std::optional<FocusedTicket> ReserveFocused(std::size_t bytes) {
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_acquire) || !bytes ||
                focusedProbes >= MaxFocusedProbes || bytes > MaxFocusedBytes - focusedBytes)
                return std::nullopt;
            focusedBytes += bytes;
            return FocusedTicket{focusedProbes++, presentIndex.load(std::memory_order_relaxed), path};
        }

        void RecordTexman(std::string line) {
            std::scoped_lock lock{mutex};
            if (active.load(std::memory_order_acquire)) {
                if (texmanEvents++ < MaxTexmanEvents)
                    AppendLocked("texman_decisions.txt", line);
            } else if (monitoring.load(std::memory_order_acquire)) {
                PushRecent(recentTexman, std::move(line), 4096);
            }
        }

        void RecordPrePass(std::string line) {
            std::scoped_lock lock{mutex};
            if (monitoring.load(std::memory_order_acquire))
                PushRecent(recentPasses, std::move(line), 1024);
        }

        void RecordScreenProbe(std::string line) {
            std::scoped_lock lock{mutex};
            if (monitoring.load(std::memory_order_acquire))
                PushRecent(recentProbes, std::move(line), 128);
            else if (!path.empty())
                AppendLocked("screen_probes.txt", line);
        }

        void RecordPrePresent(std::string line) {
            std::scoped_lock lock{mutex};
            if (monitoring.load(std::memory_order_acquire))
                PushRecent(recentPresentSources, std::move(line), 128);
        }

        void RecordPresent(std::string line) {
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_acquire))
                return;
            AppendLocked("pass_boundary.txt", std::move(line));
        }

        std::string Directory() {
            std::scoped_lock lock{mutex};
            return path;
        }
    };


    /**
     * Small visual preview of captured linear RGBA8/BGRA8 subresources.
     * No additional GPU copies: reads only the already-invalidate staging image.
     * Other formats are intentionally not guessed or decoded.
     */
    inline void WritePreview(const std::string &directory, std::uint32_t index, const char *phase,
                             const std::uint8_t *bytes, std::size_t byteCount,
                             std::uint32_t width, std::uint32_t height, bool bgra) {
        if (!width || !height || width > 8192 || height > 8192 ||
            static_cast<std::uint64_t>(width) * height * 4 > byteCount)
            return;
        const auto outWidth{std::min(width, 160U)};
        const auto outHeight{std::min(height, 90U)};
        std::ofstream ppm(std::filesystem::path(directory) /
                          (std::string(phase) + "_pass_" + std::to_string(index) + ".ppm"),
                          std::ios::binary | std::ios::trunc);
        if (!ppm)
            return;
        ppm << "P6\n" << outWidth << ' ' << outHeight << "\n255\n";
        for (std::uint32_t y{}; y < outHeight; ++y) {
            const auto sourceY{static_cast<std::uint64_t>(y) * height / outHeight};
            for (std::uint32_t x{}; x < outWidth; ++x) {
                const auto sourceX{static_cast<std::uint64_t>(x) * width / outWidth};
                const auto *pixel{bytes + 4 * (sourceY * width + sourceX)};
                const char rgb[3]{
                    static_cast<char>(pixel[bgra ? 2 : 0]),
                    static_cast<char>(pixel[1]),
                    static_cast<char>(pixel[bgra ? 0 : 2])
                };
                ppm.write(rgb, sizeof(rgb));
            }
        }
    }

    inline CosmicShakePassCapture &CosmicShakePass() {
        static CosmicShakePassCapture capture;
        return capture;
    }
}
