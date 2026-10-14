// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace skyline::gpu::diagnostic {
    /**
     * Manual, bounded diagnostic for the RE5 good->black render transition.
     * It never changes guest state or resource selection. The presentation
     * thread arms/stops it; GPU/Texman threads only record while active.
     */
    class Re5PassBoundaryCapture {
      public:
        static constexpr std::uint32_t FrameCount{6};
        static constexpr std::uint32_t MaxPasses{48};
        static constexpr std::uint32_t MaxTexmanEvents{512};
        static constexpr std::size_t MaxReadbackBytes{128ULL * 1024 * 1024};
        // Separate low-footprint sampling budget, not charged as full-image captures.
        static constexpr std::uint32_t MaxFocusedProbes{96};
        static constexpr std::size_t MaxFocusedBytes{256ULL * 1024};

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
            readbackBytes = 0;
            focusedProbes = 0;
            focusedBytes = 0;
            presentIndex.store(0, std::memory_order_relaxed);
            {
                std::ofstream(std::filesystem::path(path) / "pass_boundary.txt", std::ios::trunc)
                    << "RE5 pass-boundary diagnostic; stats are post-render-pass observations.\n";
                std::ofstream(std::filesystem::path(path) / "texman_decisions.txt", std::ios::trunc)
                    << "Bounded TextureManager decisions while capture is active.\n";
            }
            active.store(true, std::memory_order_release);
        }

        void Stop() {
            active.store(false, std::memory_order_release);
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

    inline Re5PassBoundaryCapture &Re5PassBoundary() {
        static Re5PassBoundaryCapture capture;
        return capture;
    }
}
