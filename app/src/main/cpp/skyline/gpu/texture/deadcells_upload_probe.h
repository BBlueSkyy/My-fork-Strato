// SPDX-License-Identifier: MPL-2.0
// Temporary, bounded Dead Cells display-upload provenance probe.
// Diagnostic branch only. Do not merge into production without removing it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <logger/logger.h>

namespace skyline::gpu::deadcells_probe {
    inline std::mutex mutex;
    inline std::filesystem::path directory;
    inline size_t inspected{};
    inline size_t suspiciousSaved{};
    inline size_t referenceSaved{};
    inline size_t presentsLogged{};
    inline size_t queueLogged{};
    inline bool enabled{};

    inline uint64_t Hash(const uint8_t *data, size_t length) {
        uint64_t result{14695981039346656037ULL};
        for (size_t index{}; index < length; ++index) {
            result ^= data[index];
            result *= 1099511628211ULL;
        }
        return result;
    }

    inline void Begin(const std::string &publicFilesRoot) {
        std::scoped_lock lock{mutex};
        inspected = suspiciousSaved = referenceSaved = presentsLogged = queueLogged = 0;
        enabled = false;
        std::error_code error;
        directory = std::filesystem::path{publicFilesRoot} / "logs" / "deadcells-probe";
        // Only reset the diagnostics owned by this temporary probe.
        std::filesystem::remove_all(directory, error);
        error.clear();
        std::filesystem::create_directories(directory, error);
        if (error)
            return;
        std::ofstream log{directory / "probe.jsonl", std::ios::trunc};
        if (!log)
            return;
        log << "{\"type\":\"header\",\"schema\":1,\"scope\":\"RGBA8 1280x720 CPU-to-Vulkan uploads\","
               "\"limits\":\"4 nonblank low-alpha + 2 nonblank reference samples, 120 uploads inspected\","
               "\"notes\":\"Guest bytes sampled immediately after conversion, not an IOVA trace. "
               "Texture pointers are process-local. No synchronization changes.\"}\n";
        enabled = log.good();
        if (enabled)
            LOGW("DeadCellsProbe enabled (temporary diagnostic branch); hold log icon to export ZIP");
    }

    inline bool IsCandidate(uint32_t width, uint32_t height, size_t bytes, unsigned layers, unsigned mips) {
        return width == 1280 && height == 720 && bytes == size_t(1280) * 720 * 4 &&
               layers == 1 && mips == 1;
    }

    inline bool WriteBinary(const std::filesystem::path &path, const uint8_t *bytes, size_t length) {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output || !bytes || length == 0)
            return false;
        output.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(length));
        output.close();
        if (!output.good()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return false;
        }
        return true;
    }

    // Called only after the real conversion has produced the bytes handed to the
    // upload path. Never changes memory or inserts extra GPU waits.
    inline void ObserveUpload(const void *textureIdentity,
                             const uint8_t *guestBytes, size_t guestLength,
                             const uint8_t *stageBytes, size_t stageLength,
                             int guestTileMode, unsigned guestPitch,
                             size_t guestLayerStride, size_t stagingLayerStride,
                             unsigned guestFormat, unsigned hostFormat,
                             bool hasAliases, bool guestMemoryFreed,
                             const void *guestMappingPointer) {
        if (!enabled || !guestBytes || !stageBytes || stageLength != 1280U * 720U * 4U ||
            guestLength == 0 || guestLength > 8U * 1024U * 1024U)
            return;
        std::scoped_lock lock{mutex};
        if (inspected >= 120)
            return;
        const size_t inspectedIndex{++inspected};
        size_t alpha255{};
        size_t nonzeroPixels{};
        for (size_t offset{}; offset < stageLength; offset += 4) {
            alpha255 += stageBytes[offset + 3] == 255;
            nonzeroPixels += (stageBytes[offset] | stageBytes[offset + 1] |
                              stageBytes[offset + 2] | stageBytes[offset + 3]) != 0;
        }
        const bool lowAlpha{alpha255 < (stageLength / 4) / 10};
        // Blank images are common during startup and must not consume the small
        // binary capture budget ahead of later, genuinely nonblank corruption.
        const bool save{nonzeroPixels != 0 &&
                        (lowAlpha ? suspiciousSaved < 4 : referenceSaved < 2)};
        const uint64_t guestHash{Hash(guestBytes, guestLength)};
        const uint64_t stagingHash{Hash(stageBytes, stageLength)};
        std::string guestFile, stagingFile;
        if (save) {
            if (lowAlpha) ++suspiciousSaved;
            else ++referenceSaved;
            const std::string prefix{"sample_" + std::to_string(inspectedIndex)};
            guestFile = prefix + "_guest.bin";
            stagingFile = prefix + "_staging.bin";
            if (!WriteBinary(directory / guestFile, guestBytes, guestLength))
                guestFile.clear();
            if (!WriteBinary(directory / stagingFile, stageBytes, stageLength))
                stagingFile.clear();
        }

        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"upload\",\"index\":" << inspectedIndex
                << ",\"texture_ptr\":\"" << textureIdentity << "\""
                << ",\"width\":1280,\"height\":720,\"staging_bytes\":" << stageLength
                << ",\"guest_bytes\":" << guestLength
                << ",\"guest_mapping_ptr\":\"" << guestMappingPointer << "\""
                << ",\"nonzero_pixels\":" << nonzeroPixels
                << ",\"opaque_alpha_pixels\":" << alpha255
                << ",\"total_pixels\":921600,\"low_alpha\":" << (lowAlpha ? "true" : "false")
                << ",\"guest_fnv64\":" << guestHash << ",\"staging_fnv64\":" << stagingHash
                << ",\"tile_mode\":" << guestTileMode
                << ",\"guest_pitch\":" << guestPitch
                << ",\"guest_layer_stride\":" << guestLayerStride
                << ",\"staging_layer_stride\":" << stagingLayerStride
                << ",\"guest_format\":" << guestFormat
                << ",\"host_format\":" << hostFormat
                << ",\"aliases\":" << (hasAliases ? "true" : "false")
                << ",\"guest_memory_freed\":" << (guestMemoryFreed ? "true" : "false")
                << ",\"sync_path\":\"SynchronizeHostImpl\""
                << ",\"guest_bin\":\"" << guestFile
                << "\",\"staging_bin\":\"" << stagingFile << "\"}\n";
        if (save)
            LOGW("DeadCellsProbe upload #{} texture={} lowAlpha={} opaque={}/921600 guestHash={:#x} stagedHash={:#x}",
                 inspectedIndex, textureIdentity, lowAlpha, alpha255, guestHash, stagingHash);
    }


    inline void ObserveQueue(const void *textureIdentity, const void *guestMappingPointer,
                             uint32_t width, uint32_t height, int slot,
                             uint64_t frameNumber, uint32_t nvmapHandleId,
                             uint64_t surfaceOffset, uint64_t surfaceSize,
                             unsigned surfaceLayout, unsigned blockHeightLog2) {
        if (!enabled || width != 1280 || height != 720) return;
        std::scoped_lock lock{mutex};
        if (queueLogged >= 600) return;
        const size_t seq{++queueLogged};
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"queue\",\"index\":" << seq
                << ",\"texture_ptr\":\"" << textureIdentity << "\""
                << ",\"guest_mapping_ptr\":\"" << guestMappingPointer << "\""
                << ",\"slot\":" << slot
                << ",\"frame_number\":" << frameNumber
                << ",\"nvmap_handle\":" << nvmapHandleId
                << ",\"surface_offset\":" << surfaceOffset
                << ",\"surface_size\":" << surfaceSize
                << ",\"surface_layout\":" << surfaceLayout
                << ",\"block_height_log2\":" << blockHeightLog2
                << "}\n";
    }

    inline void ObservePresent(const void *textureIdentity, uint32_t width, uint32_t height,
                               bool guestMapped) {
        if (!enabled || width != 1280 || height != 720)
            return;
        std::scoped_lock lock{mutex};
        if (presentsLogged++ >= 600)
            return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"present\",\"index\":" << presentsLogged
                << ",\"texture_ptr\":\"" << textureIdentity
                << "\",\"guest_mapped\":" << (guestMapped ? "true" : "false")
                << "}\n";
    }
} // namespace skyline::gpu::deadcells_probe
