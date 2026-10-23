// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sparking_upload_provenance.h"
// One-shot diagnostic only. This is a small, bounded trace, not the RenderDoc API.
// No guest data or guest GPU command is modified.
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <span>
#include <initializer_list>
#include <unordered_set>

namespace skyline::gpu::diagnostic {
    struct SparkingHdrStats {
        std::size_t nanPixels{}, infPixels{};
        static SparkingHdrStats Decode(std::span<const std::uint8_t> bytes) {
            SparkingHdrStats result;
            for (std::size_t i{}; i + 4 <= bytes.size(); i += 4) {
                const std::uint32_t pixel{std::uint32_t(bytes[i]) | (std::uint32_t(bytes[i+1]) << 8) |
                    (std::uint32_t(bytes[i+2]) << 16) | (std::uint32_t(bytes[i+3]) << 24)};
                bool nan{}, inf{};
                for (const auto &[shift, bits] : {std::pair{0U, 6U}, {11U, 6U}, {22U, 5U}}) {
                    const auto field{pixel >> shift};
                    if (((field >> bits) & 31) != 31) continue;
                    if (field & ((1U << bits) - 1)) nan = true;
                    else inf = true;
                }
                result.nanPixels += nan;
                result.infPixels += inf;
            }
            return result;
        }
        static SparkingHdrStats Decode(std::initializer_list<std::uint8_t> bytes) {
            return Decode(std::span<const std::uint8_t>{bytes.begin(), bytes.size()});
        }
    };

    enum class SparkingFocusRole : std::uint8_t {
        SceneHdr,
        ToneHdrInput,
        ToneLdrOutput,
        PostLdrInput,
        PostLdrOutput,
        UpscaleLdrInput,
        UpscaleLdrOutput,
        Count,
    };

    constexpr const char *SparkingFocusName(SparkingFocusRole role) {
        switch (role) {
            case SparkingFocusRole::SceneHdr: return "scene_hdr";
            case SparkingFocusRole::ToneHdrInput: return "tonemap_hdr_input";
            case SparkingFocusRole::ToneLdrOutput: return "tonemap_ldr_output";
            case SparkingFocusRole::PostLdrInput: return "post_ldr_input";
            case SparkingFocusRole::PostLdrOutput: return "post_ldr_output";
            case SparkingFocusRole::UpscaleLdrInput: return "upscale_ldr_input";
            case SparkingFocusRole::UpscaleLdrOutput: return "upscale_ldr_output";
            case SparkingFocusRole::Count: break;
        }
        return "unknown";
    }

    class SparkingFrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<bool> hdrDrawScan{false};
        static constexpr std::uint32_t MaxHdrDraws{192};
        static constexpr std::size_t MaxScanBytes{384ULL * 1024 * 1024};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{12000};
        static constexpr std::uint32_t FrameCount{6};
        // Reserve separate quotas for HDR, tone mapping, LDR postprocess and upscale.
        static constexpr std::uint32_t PassSnapshotsPerInterval{8};
        static constexpr std::uint32_t MaxPassSnapshots{48};
        static constexpr std::size_t MaxPassCaptureBytes{160ULL * 1024 * 1024};

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::array<std::array<std::uint8_t, static_cast<std::size_t>(SparkingFocusRole::Count)>, FrameCount> roleCounts{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        std::uintptr_t scanTexture{}, scanExecutor{};
        std::uint32_t scanDraw{};
        std::size_t scanBytes{}, inputBytes{};
        std::unordered_set<std::uintptr_t> scanInputs;
        std::filesystem::path shaderDirectory;
        std::atomic<bool> shaderDumpEnabled{false};
        std::size_t shaderBytes{};
        std::unordered_set<std::string> dumpedShaders;
        // Guest backing ranges shared across Maxwell3D, Fermi2D and VI executors.
        // No texture pointers, shader hashes or game IDs are used as persistent keys.
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;

      public:
        void Start(std::string captureDirectory) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            std::error_code ec;
            hdrDrawScan.store(std::filesystem::exists(std::filesystem::path(path).parent_path() /
                "ENABLE_HDR_DRAW_SCAN", ec) && !ec, std::memory_order_release);
            scanTexture = scanExecutor = scanDraw = scanBytes = inputBytes = 0;
            scanInputs.clear();
            if (hdrDrawScan.load(std::memory_order_relaxed)) {
                std::ofstream csv(std::filesystem::path(path) / "hdr_draw_stats.csv", std::ios::trunc);
                csv << "ordinal,snapshot,nan_pixels,inf_pixels\n";
            }
            events.clear();
            passCountByInterval.fill(0);
            for (auto &counts : roleCounts)
                counts.fill(0);
            nextPassIndex = 0;
            passBytes = 0;
            watchedMappings.clear();
            eventCount.store(0, std::memory_order_relaxed);
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        std::optional<std::uint32_t> ClaimHdrDraw(std::uintptr_t texture, std::uintptr_t executor = 0) {
            if (!active.load(std::memory_order_acquire) || !hdrDrawScan.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || !texture || scanDraw >= MaxHdrDraws)
                return std::nullopt;
            if (!scanTexture) { scanTexture = texture; scanExecutor = executor; }
            if (texture != scanTexture || executor != scanExecutor) return std::nullopt;
            return ++scanDraw;
        }

        bool ReserveScanBytes(std::size_t bytes) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || !bytes || bytes > MaxScanBytes - scanBytes)
                return false;
            scanBytes += bytes;
            return true;
        }

        bool ClaimScanInput(std::uintptr_t texture, std::size_t bytes) {
            std::scoped_lock lock(mutex);
            constexpr std::size_t InputLimit{48ULL * 1024 * 1024};
            if (!active.load(std::memory_order_relaxed) || scanInputs.size() >= 64 ||
                scanInputs.contains(texture) || !bytes || bytes > InputLimit - inputBytes)
                return false;
            inputBytes += bytes;
            scanInputs.insert(texture);
            return true;
        }

        // Configured once before game shader compilation. Never poll disk per draw.
        void ConfigureShaderDump(const std::filesystem::path &root) {
            std::scoped_lock lock(mutex);
            std::error_code ec;
            shaderDumpEnabled = std::filesystem::exists(root / "ENABLE_HDR_DRAW_SCAN", ec) && !ec;
            SparkingUploads().enabled.store(shaderDumpEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
            shaderDirectory = root / "scan_shaders";
            if (shaderDumpEnabled) std::filesystem::create_directories(shaderDirectory, ec);
            if (ec) shaderDumpEnabled = false;
        }

        void DumpShader(std::uint64_t hash, bool spirv, std::span<const std::uint8_t> bytes) {
            if (!shaderDumpEnabled.load(std::memory_order_relaxed)) return;
            std::scoped_lock lock(mutex);
            constexpr std::size_t Limit{64ULL * 1024 * 1024};
            if (!shaderDumpEnabled || bytes.empty() || dumpedShaders.size() >= 2048 || bytes.size() > Limit - shaderBytes)
                return;
            std::uint64_t contentHash{14695981039346656037ULL};
            for (auto byte : bytes) { contentHash ^= byte; contentHash *= 1099511628211ULL; }
            const auto name{std::to_string(hash) + "_" + std::to_string(contentHash) + (spirv ? ".spv" : ".guest")};
            if (!dumpedShaders.insert(name).second) return;
            shaderBytes += bytes.size();
            std::ofstream out(shaderDirectory / name, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            std::ofstream manifest(shaderDirectory / "shader_manifest.txt", std::ios::app);
            manifest << "guest_hash_decimal=" << hash << " file=" << name << " bytes=" << bytes.size()
                     << " write_ok=" << static_cast<bool>(out) << '\n';
        }

        // Allocates a unique, globally capped snapshot ID across all executors.
        // Called before allocating staging buffers or writing file names.
        std::optional<std::uint32_t> ReservePassSnapshot(std::uint32_t interval, std::size_t bytes,
                                                        SparkingFocusRole role) {
            std::scoped_lock lock(mutex);
            const auto roleIndex{static_cast<std::size_t>(role)};
            if (!active.load(std::memory_order_acquire) ||
                hdrDrawScan.load(std::memory_order_relaxed) || interval >= FrameCount || roleIndex >= static_cast<std::size_t>(SparkingFocusRole::Count) ||
                bytes == 0 || passCountByInterval[interval] >= PassSnapshotsPerInterval ||
                nextPassIndex >= MaxPassSnapshots || bytes > MaxPassCaptureBytes ||
                passBytes > MaxPassCaptureBytes - bytes)
                return std::nullopt;
            // Earlier capture spent all six slots on repeated 1024x256 auxiliaries.
            // Reserve one per role, except two independent HDR scene outputs.
            const std::uint8_t limit{role == SparkingFocusRole::SceneHdr ? std::uint8_t{2} : std::uint8_t{1}};
            if (roleCounts[interval][roleIndex] >= limit)
                return std::nullopt;
            ++roleCounts[interval][roleIndex];
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
            const auto limit{hdrDrawScan.load(std::memory_order_acquire) ? 64000U : MaxEvents};
            if (!active.load(std::memory_order_acquire) ||
                eventCount.load(std::memory_order_relaxed) >= limit)
                return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || events.size() >= limit)
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
                    << "Events are capped at 12000 (64000 in HDR draw scan mode).\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
        }
    };

    inline SparkingFrameCapture &SparkingCapture() {
        static SparkingFrameCapture session;
        return session;
    }
}