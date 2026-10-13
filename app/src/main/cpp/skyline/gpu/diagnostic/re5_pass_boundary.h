// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
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

        struct PassTicket {
            std::uint32_t index{};
            std::uint32_t present{};
            std::string directory;
        };

        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};

      private:
        std::mutex mutex;
        std::string path;
        std::uint32_t nextPass{};
        std::uint32_t texmanEvents{};
        std::size_t readbackBytes{};

        void AppendLocked(const char *name, const std::string &line) {
            if (path.empty())
                return;
            std::ofstream out(std::filesystem::path(path) / name, std::ios::app);
            if (out)
                out << line << '\n';
        }

      public:
        void Start(std::string directory) {
            std::scoped_lock lock{mutex};
            path = std::move(directory);
            nextPass = 0;
            texmanEvents = 0;
            readbackBytes = 0;
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

        void RecordTexman(std::string line) {
            std::scoped_lock lock{mutex};
            if (!active.load(std::memory_order_acquire) || texmanEvents >= MaxTexmanEvents)
                return;
            ++texmanEvents;
            AppendLocked("texman_decisions.txt", line);
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
