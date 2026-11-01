// SPDX-License-Identifier: MPL-2.0
#pragma once

// Manual, bounded PAC-MAN geometry investigation. This is NOT a RenderDoc
// capture file; a real .rdc additionally requires an installed RenderDoc layer.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace skyline::gpu::diagnostic {
    class PacmanTriangleCapture {
        static constexpr std::uint32_t MaxEvents{12000};
        static constexpr std::uint32_t MaxPresentedFrames{6};

        mutable std::mutex mutex;
        std::filesystem::path destination;
        std::vector<std::string> events;
        std::atomic<bool> active{false};
        std::atomic<bool> requestRenderDoc{false};
        std::atomic<std::uint32_t> presentedFrames{0};
        std::uint32_t droppedEvents{};

      public:
        bool Active() const {
            return active.load(std::memory_order_acquire);
        }

        std::uint32_t PresentedFrames() const {
            return presentedFrames.load(std::memory_order_relaxed);
        }

        void Start(std::filesystem::path folder) {
            std::lock_guard lock{mutex};
            destination = std::move(folder);
            events.clear();
            events.reserve(MaxEvents);
            droppedEvents = 0;
            presentedFrames.store(0, std::memory_order_relaxed);
            requestRenderDoc.store(true, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        void Record(std::string event) {
            if (!Active())
                return;
            std::lock_guard lock{mutex};
            if (!Active())
                return;
            if (events.size() < MaxEvents)
                events.emplace_back(std::move(event));
            else
                ++droppedEvents;
        }

        // Exactly one command-executor submission when an Android RenderDoc
        // Vulkan layer is loaded. The text trace works without that layer.
        bool TakeRenderDocRequest() {
            return Active() && requestRenderDoc.exchange(false, std::memory_order_acq_rel);
        }

        // Called from the presentation thread after recording metadata of the
        // actual source texture, before the host swapchain copy.
        void CompletePresentation() {
            if (!Active())
                return;
            if (presentedFrames.fetch_add(1, std::memory_order_acq_rel) + 1 <
                MaxPresentedFrames)
                return;

            std::lock_guard lock{mutex};
            if (!active.exchange(false, std::memory_order_acq_rel))
                return;
            std::ofstream trace{destination / "gpu_events.txt", std::ios::trunc};
            if (trace) {
                trace << "PAC-MAN WORLD Re-PAC mini diagnostic, NOT an RDC file\n"
                      << "present intervals approximate; GPU submissions are asynchronous\n";
                for (const auto &line : events)
                    trace << line << '\n';
                trace << "dropped_events=" << droppedEvents << '\n';
            }
            std::ofstream completion{destination / "capture_complete.txt", std::ios::trunc};
            if (completion)
                completion << "recorded_presentations=" << MaxPresentedFrames << '\n'
                           << "events_written=" << events.size() << '\n'
                           << "dropped_events=" << droppedEvents << '\n'
                           << "metadata_only=true\n";
        }
    };

    inline PacmanTriangleCapture &PacmanCapture() {
        static PacmanTriangleCapture capture;
        return capture;
    }
}
