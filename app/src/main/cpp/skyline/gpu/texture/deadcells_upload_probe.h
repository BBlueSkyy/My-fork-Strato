// SPDX-License-Identifier: MPL-2.0
// Temporary, bounded Dead Cells display-upload provenance probe.
// Diagnostic branch only. Do not merge into production without removing it.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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
    inline size_t producerEventsLogged{};
    inline size_t gpuTextureWriterEvents{};
    struct WatchedSurface {
        uintptr_t hostStart{};
        uintptr_t hostEnd{};
        uint64_t guestAddress{};
        size_t sampledBytes{};
        uint64_t initialSparseHash{};
        uint64_t initialHeaderAt16{};
        uint64_t initialHeaderAt32{};
    };
    inline std::array<WatchedSurface, 24> watchedSurfaces{};
    inline size_t watchedCount{};
    inline size_t allocSnapshotCount{};
    inline uint64_t timelineSequence{};
    inline size_t timelineLayers{}, timelineSlots{}, timelineTextures{}, timelineNvmaps{}, timelinePresents{};
    // Rolling history of potential GPU-side writers. It is intentionally
    // metadata-only and makes no claims about GPU submission/completion.
    struct PriorWriter {
        uintptr_t start{};
        uintptr_t end{};
        const void *identity{};
        const char *kind{};
        uint64_t eventSerial{};
    };
    inline std::array<PriorWriter, 2048> priorWriters{};
    inline uint64_t priorWriterSequence{};
    inline size_t priorWriterCount{};
    inline std::array<uint64_t, 24> allocatedAddresses{};
    inline bool enabled{};

    inline uint64_t Hash(const uint8_t *data, size_t length) {
        uint64_t result{14695981039346656037ULL};
        for (size_t index{}; index < length; ++index) {
            result ^= data[index];
            result *= 1099511628211ULL;
        }
        return result;
    }

    // A cheap, bounded cross-stage fingerprint. Only ~6 KiB are read per
    // 1280x720 surface. A matching sample is NOT proof that all bytes match.
    inline uint64_t SparseHash(const uint8_t *data, size_t bytes) {
        constexpr size_t BlockSize{256};
        constexpr size_t Blocks{24};
        if (!data || bytes < BlockSize) return 0;
        uint64_t hash{14695981039346656037ULL};
        for (size_t block{}; block < Blocks; ++block) {
            const size_t offset{(bytes - BlockSize) * block / (Blocks - 1)};
            for (size_t j{}; j < BlockSize; ++j) {
                hash ^= data[offset + j];
                hash *= 1099511628211ULL;
            }
        }
        return hash;
    }

    inline void Begin(const std::string &publicFilesRoot) {
        std::scoped_lock lock{mutex};
        inspected = suspiciousSaved = referenceSaved = presentsLogged = queueLogged = 0;
        producerEventsLogged = gpuTextureWriterEvents = watchedCount = 0;
        watchedSurfaces.fill({});
        allocSnapshotCount = 0;
        timelineSequence = timelineLayers = timelineSlots = timelineTextures = timelineNvmaps = timelinePresents = 0;
        allocatedAddresses.fill(0);
        priorWriterSequence = priorWriterCount = 0;
        priorWriters.fill({});
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
        log << "{\"type\":\"header\",\"schema\":2,\"scope\":\"bounded causal timeline + RGBA8 1280x720 uploads\","
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





    // Independently capped, metadata-only timeline. Steady-clock timestamps
    // order host observations; they are NOT GPU execution timestamps.
    inline uint64_t TimestampUs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline void TraceLayerEvent(const char *action, const void *driver, uint64_t layerId,
                                uint64_t displayId, const void *producer, bool reused) {
        if (!enabled) return;
        std::scoped_lock lock{mutex};
        if (timelineLayers++ >= 512) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"timeline_layer\",\"event_seq\":" << ++timelineSequence
                << ",\"timestamp_us\":" << TimestampUs()
                << ",\"action\":\"" << action << "\""
                << ",\"driver_ptr\":\"" << driver << "\""
                << ",\"layer_id\":" << layerId << ",\"display_id\":" << displayId
                << ",\"producer_ptr\":\"" << producer << "\""
                << ",\"reused\":" << (reused ? "true" : "false") << "}\n";
    }

    inline void TraceSlotEvent(const char *action, const void *producer, int slot,
                               uint64_t frame, int state, uint32_t nvmapHandle = 0,
                               const void *texture = nullptr, uint32_t width = 0,
                               uint32_t height = 0) {
        if (!enabled) return;
        std::scoped_lock lock{mutex};
        if (timelineSlots++ >= 2400) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"timeline_slot\",\"event_seq\":" << ++timelineSequence
                << ",\"timestamp_us\":" << TimestampUs()
                << ",\"action\":\"" << action << "\""
                << ",\"producer_ptr\":\"" << producer << "\""
                << ",\"slot\":" << slot << ",\"frame_number\":" << frame
                << ",\"slot_state\":" << state
                << ",\"nvmap_handle\":" << nvmapHandle
                << ",\"texture_ptr\":\"" << texture << "\""
                << ",\"width\":" << width << ",\"height\":" << height << "}\n";
    }

    inline void TraceTextureDecision(const char *action, const void *producer,
                                     int slot, uint64_t frame, uint32_t handle,
                                     const void *texture, const void *mapping,
                                     bool createdView) {
        if (!enabled) return;
        std::scoped_lock lock{mutex};
        if (timelineTextures++ >= 600) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"timeline_texture\",\"event_seq\":" << ++timelineSequence
                << ",\"timestamp_us\":" << TimestampUs()
                << ",\"action\":\"" << action << "\""
                << ",\"producer_ptr\":\"" << producer << "\""
                << ",\"slot\":" << slot << ",\"frame_number\":" << frame
                << ",\"nvmap_handle\":" << handle
                << ",\"texture_ptr\":\"" << texture << "\""
                << ",\"host_mapping_ptr\":\"" << mapping << "\""
                << ",\"created_view_in_queue\":"
                << (createdView ? "true" : "false") << "}\n";
    }

    inline void TraceNvmapEvent(const char *action, uint32_t handle,
                                uint64_t guestAddr, size_t bytes, bool success) {
        if (!enabled || bytes < 2U * 1024U * 1024U ||
            bytes > 16U * 1024U * 1024U) return;
        std::scoped_lock lock{mutex};
        if (timelineNvmaps++ >= 1000) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"timeline_nvmap\",\"event_seq\":" << ++timelineSequence
                << ",\"timestamp_us\":" << TimestampUs()
                << ",\"action\":\"" << action << "\""
                << ",\"nvmap_handle\":" << handle
                << ",\"guest_address\":" << guestAddr
                << ",\"allocation_bytes\":" << bytes
                << ",\"success\":" << (success ? "true" : "false") << "}\n";
    }

    inline void TracePresentStage(const char *action, uint64_t frameId,
                                  const void *texture) {
        if (!enabled) return;
        std::scoped_lock lock{mutex};
        if (timelinePresents++ >= 1800) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"timeline_present_stage\",\"event_seq\":"
                << ++timelineSequence
                << ",\"timestamp_us\":" << TimestampUs()
                << ",\"action\":\"" << action << "\""
                << ",\"presentation_frame_id\":" << frameId
                << ",\"texture_ptr\":\"" << texture << "\"}\n";
    }

    // Record GPU writer intents by host-mapping span. A host pointer is not a
    // guest 0x18... address; a guest-address prefix filter loses real writers.
    // No guest pixels read; 2048 entries bound retained history.
    inline void ObservePriorWriter(const char *kind, const void *identity,
                                  const void *mapping, size_t bytes) {
        if (!enabled || !mapping || !bytes) return;
        const uintptr_t start{reinterpret_cast<uintptr_t>(mapping)};
        if (bytes > UINTPTR_MAX - start || bytes > 256U * 1024U * 1024U)
            return;
        std::scoped_lock lock{mutex};
        const uint64_t serial{++priorWriterSequence};
        priorWriters[(serial - 1) % priorWriters.size()] =
            {start, start + bytes, identity, kind, serial};
        if (priorWriterCount < priorWriters.size()) ++priorWriterCount;
    }

    // Snapshot only the 3.985 MiB NvMap allocations observed as display
    // backings in Dead Cells. This is before HOS binder attaches a surface.
    // A guest-supplied VA is *not* a producer identity; a matching hash simply
    // narrows the interval in which the bytes first became visible.
    inline void ObserveNvmapAllocation(uint32_t handleId, uint64_t guestAddress,
                                       size_t allocationBytes, const uint8_t *hostBytes) {
        constexpr size_t TargetAllocationBytes{3985408};
        if (!enabled || allocationBytes != TargetAllocationBytes || !hostBytes || !guestAddress)
            return;
        std::scoped_lock lock{mutex};
        for (size_t i{}; i < allocSnapshotCount; ++i)
            if (allocatedAddresses[i] == guestAddress) return;
        if (allocSnapshotCount >= allocatedAddresses.size()) return;
        allocatedAddresses[allocSnapshotCount++] = guestAddress;
        const size_t sampleLength{1280U * 720U * 4U};
        // Look backwards for GPU writes targeting the same host mapping,
        // including writers that predate the display texture or NvMap handle.
        const uintptr_t start{reinterpret_cast<uintptr_t>(hostBytes)};
        const uintptr_t end{start + allocationBytes};
        uint64_t lastWriterEvent{}, overlapCount{}, lastOverlapBytes{};
        const void *lastWriterIdentity{};
        const char *lastWriterKind{"none"};
        for (size_t n{}; n < priorWriterCount; ++n) {
            const auto &writer{priorWriters[n]};
            if (writer.start >= end || writer.end <= start) continue;
            ++overlapCount;
            if (writer.eventSerial > lastWriterEvent) {
                lastWriterEvent = writer.eventSerial;
                lastWriterIdentity = writer.identity;
                lastWriterKind = writer.kind;
                lastOverlapBytes = std::min(end, writer.end) -
                                   std::max(start, writer.start);
            }
        }
        const uint64_t hash{SparseHash(hostBytes, sampleLength)};
        uint64_t header32{};
        std::memcpy(&header32, hostBytes + 32, sizeof(header32));
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"nvmap_alloc_snapshot\""
                << ",\"nvmap_handle\":" << handleId
                << ",\"guest_address\":" << guestAddress
                << ",\"allocation_bytes\":" << allocationBytes
                << ",\"sparse_sample_bytes\":6144"
                << ",\"sparse_hash_at_allocation\":" << hash
                << ",\"guest_header_u64_at32\":" << header32
                << ",\"prior_writer_overlap_count\":" << overlapCount
                << ",\"prior_writer_latest_kind\":\"" << lastWriterKind << "\""
                << ",\"prior_writer_latest_identity\":\"" << lastWriterIdentity << "\""
                << ",\"prior_writer_latest_event\":" << lastWriterEvent
                << ",\"prior_writer_latest_overlap_bytes\":" << lastOverlapBytes
                << ",\"prior_writer_history_events_seen\":" << priorWriterSequence
                << ",\"prior_writer_history_capacity\":" << priorWriters.size()
                << ",\"stage\":\"after_nvmap_alloc_before_surface_registration\""
                << "}\n";
    }

    // Called at surface attachment/preallocation, before queue or upload. This
    // records only address-space metadata; it does not read the guest pixels.
    inline void RegisterDisplayBacking(const void *hostMapping, size_t bytes,
                                       uint64_t guestAddress) {
        if (!enabled || !hostMapping || !bytes || bytes > 8U * 1024U * 1024U)
            return;
        const uintptr_t start{reinterpret_cast<uintptr_t>(hostMapping)};
        if (start > UINTPTR_MAX - bytes) return;
        std::scoped_lock lock{mutex};
        for (size_t n{}; n < watchedCount; ++n)
            if (watchedSurfaces[n].hostStart == start &&
                watchedSurfaces[n].hostEnd == start + bytes)
                return;
        if (watchedCount < watchedSurfaces.size()) {
            const size_t sampleBytes{std::min<size_t>(bytes, 1280U * 720U * 4U)};
            uint64_t headerAt16{}, headerAt32{};
            if (sampleBytes >= 24)
                std::memcpy(&headerAt16, reinterpret_cast<const uint8_t *>(hostMapping) + 16, 8);
            if (sampleBytes >= 40)
                std::memcpy(&headerAt32, reinterpret_cast<const uint8_t *>(hostMapping) + 32, 8);
            const uint64_t initialHash{
                SparseHash(reinterpret_cast<const uint8_t *>(hostMapping), sampleBytes)
            };
            watchedSurfaces[watchedCount++] = {
                start, start + bytes, guestAddress, sampleBytes, initialHash, headerAt16, headerAt32
            };
        }
    }

    // Records a planned DMA destination when it overlaps a previously
    // registered display allocation. This does NOT prove that DMA completed.
    inline void ObserveMappedDmaIntent(const char *operation, const void *dst,
                                      size_t bytes, bool gpuBufferWriter = false) {
        if (!enabled || !dst || !bytes) return;
        ObservePriorWriter(gpuBufferWriter ? "gpu_buffer" : operation, nullptr,
                           dst, bytes);
        const uintptr_t start{reinterpret_cast<uintptr_t>(dst)};
        if (start > UINTPTR_MAX - bytes) return;
        std::scoped_lock lock{mutex};
        if (producerEventsLogged >= 192) return;
        for (size_t n{}; n < watchedCount; ++n) {
            const auto &watch{watchedSurfaces[n]};
            if (start >= watch.hostEnd || start + bytes <= watch.hostStart)
                continue;
            std::ofstream log{directory / "probe.jsonl", std::ios::app};
            if (log)
                log << "{\"type\":\""
                    << (gpuBufferWriter ? "gpu_buffer_write_intent" : "dma_write_intent")
                    << "\",\"index\":" << ++producerEventsLogged
                    << ",\"engine\":\"" << operation << "\""
                    << ",\"host_destination\":\"" << dst << "\""
                    << ",\"write_size\":" << bytes
                    << ",\"watched_guest_address\":" << watch.guestAddress
                    << ",\"intersection_start_offset\":"
                    << (std::max(start, watch.hostStart) - watch.hostStart)
                    << ",\"intersection_bytes\":"
                    << (std::min(start + bytes, watch.hostEnd) -
                        std::max(start, watch.hostStart))
                    << ",\"status\":\"recorded_before_submission_not_completion\"}\n";
            return;
        }
    }

    inline void ObserveGpuTextureWriterIntent(const void *textureIdentity,
                                              uint32_t width, uint32_t height,
                                              uint64_t writerSerial) {
        if (!enabled || width != 1280 || height != 720) return;
        std::scoped_lock lock{mutex};
        if (gpuTextureWriterEvents >= 96) return;
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"gpu_texture_write_intent\",\"index\":"
                << ++gpuTextureWriterEvents
                << ",\"texture_ptr\":\"" << textureIdentity << "\""
                << ",\"gpu_write_serial\":" << writerSerial
                << ",\"status\":\"writer_classified_not_completed\"}\n";
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
                             const void *guestMappingPointer,
                             uint64_t cpuWriteTrapHits, uint64_t gpuWriteSerial,
                             bool everUsedAsGpuWrite) {
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
                << ",\"cpu_write_trap_hits\":" << cpuWriteTrapHits
                << ",\"last_gpu_write_serial\":" << gpuWriteSerial
                << ",\"ever_used_as_gpu_writer\":" << (everUsedAsGpuWrite ? "true" : "false")
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


    inline void ObserveQueue(const void *textureIdentity,
                             uint32_t width, uint32_t height, int slot,
                             uint64_t frameNumber, uint32_t nvmapHandleId,
                             uint64_t nvmapGuestAddress, uint64_t nvmapSize,
                             uint64_t surfaceOffset, uint64_t surfaceSize,
                             unsigned surfaceLayout, unsigned blockHeightLog2,
                             unsigned fenceCount, uint32_t firstFenceId,
                             uint32_t firstFenceThreshold) {
        if (!enabled || width != 1280 || height != 720) return;
        std::scoped_lock lock{mutex};
        if (queueLogged >= 600) return;
        const size_t seq{++queueLogged};
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"queue\",\"index\":" << seq
                << ",\"texture_ptr\":\"" << textureIdentity << "\""
                << ",\"slot\":" << slot
                << ",\"frame_number\":" << frameNumber
                << ",\"nvmap_handle\":" << nvmapHandleId
                << ",\"nvmap_guest_address\":" << nvmapGuestAddress
                << ",\"nvmap_allocation_bytes\":" << nvmapSize
                << ",\"surface_offset\":" << surfaceOffset
                << ",\"surface_size\":" << surfaceSize
                << ",\"surface_layout\":" << surfaceLayout
                << ",\"block_height_log2\":" << blockHeightLog2
                << ",\"fence_count\":" << fenceCount
                << ",\"first_fence_id\":" << firstFenceId
                << ",\"first_fence_threshold\":" << firstFenceThreshold
                << "}\n";
    }

    inline void ObservePresent(const void *textureIdentity, uint32_t width, uint32_t height,
                               bool guestMapped, const void *guestMappingPtr,
                               const uint8_t *guestMirror, size_t mirrorBytes) {
        if (!enabled || width != 1280 || height != 720)
            return;
        std::scoped_lock lock{mutex};
        if (presentsLogged >= 600)
            return;
        const size_t index{++presentsLogged};
        bool registrationFound{};
        uint64_t initialHash{}, currentHash{}, initialHeader{}, currentHeader{};
        uint64_t initialHeader32{}, currentHeader32{};
        uint64_t registeredGuestAddress{};
        if (guestMappingPtr && guestMirror) {
            const auto mappingAddress{reinterpret_cast<uintptr_t>(guestMappingPtr)};
            for (size_t n{}; n < watchedCount; ++n) {
                const auto &watch{watchedSurfaces[n]};
                if (watch.hostStart != mappingAddress ||
                    mirrorBytes < watch.sampledBytes ||
                    watch.sampledBytes < 256)
                    continue;
                registrationFound = true;
                registeredGuestAddress = watch.guestAddress;
                initialHash = watch.initialSparseHash;
                currentHash = SparseHash(guestMirror, watch.sampledBytes);
                initialHeader = watch.initialHeaderAt16;
                initialHeader32 = watch.initialHeaderAt32;
                if (watch.sampledBytes >= 24)
                    std::memcpy(&currentHeader, guestMirror + 16, 8);
                if (watch.sampledBytes >= 40)
                    std::memcpy(&currentHeader32, guestMirror + 32, 8);
                break;
            }
        }
        std::ofstream log{directory / "probe.jsonl", std::ios::app};
        if (log)
            log << "{\"type\":\"present\",\"index\":" << index
                << ",\"texture_ptr\":\"" << textureIdentity
                << "\",\"guest_mapped\":" << (guestMapped ? "true" : "false")
                << ",\"watch_matched\":" << (registrationFound ? "true" : "false")
                << ",\"registered_guest_address\":" << registeredGuestAddress
                << ",\"sparse_hash_on_registration\":" << initialHash
                << ",\"sparse_hash_after_fence\":" << currentHash
                << ",\"sparse_sample_changed\":"
                << (registrationFound && initialHash != currentHash ? "true" : "false")
                << ",\"header_u64_at16_on_registration\":" << initialHeader
                << ",\"header_u64_at16_after_fence\":" << currentHeader
                << ",\"header_u64_at32_on_registration\":" << initialHeader32
                << ",\"header_u64_at32_after_fence\":" << currentHeader32
                << ",\"snapshot_stage\":\"after_original_frame_fence_wait_before_upload\""
                << "}\n";
    }

} // namespace skyline::gpu::deadcells_probe
