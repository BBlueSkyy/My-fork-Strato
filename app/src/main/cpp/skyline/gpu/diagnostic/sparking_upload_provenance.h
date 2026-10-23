// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <sstream>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace skyline::gpu::diagnostic {
    // Diagnostic destination envelopes, not a claim that every byte was written.
    // Saturation keeps malformed/large register values from wrapping ranges.
    constexpr std::uint64_t SparkingSaturatingAdd(std::uint64_t a, std::uint64_t b) {
        return b > std::numeric_limits<std::uint64_t>::max() - a ?
            std::numeric_limits<std::uint64_t>::max() : a + b;
    }
    constexpr std::uint64_t SparkingSaturatingMultiply(std::uint64_t a, std::uint64_t b) {
        return a && b > std::numeric_limits<std::uint64_t>::max() / a ?
            std::numeric_limits<std::uint64_t>::max() : a * b;
    }
    constexpr std::uint64_t SparkingPitchExtent(std::uint64_t line, std::uint64_t count, std::uint64_t pitch) {
        return count ? SparkingSaturatingAdd(SparkingSaturatingMultiply(count - 1, pitch), line) : 0;
    }
    constexpr std::uint64_t SparkingBlockExtent(std::uint64_t width, std::uint64_t height,
                                               std::uint64_t depth, std::uint64_t bh, std::uint64_t bd) {
        const auto align=[](std::uint64_t n,std::uint64_t a) {
            return a ? SparkingSaturatingMultiply(n / a + (n % a != 0), a) : 0;
        };
        return SparkingSaturatingMultiply(SparkingSaturatingMultiply(align(width,64),align(height,8*bh)),align(depth,bd));
    }

    class SparkingTransferTrace {
      public:
        static constexpr size_t Capacity{4096};
        struct Event {
            const char *kind{};
            std::uintptr_t addressSpace{};
            std::uint64_t source{}, destination{}, bytes{}, sequence{};
            std::array<std::uint32_t,20> words{};
            size_t wordCount{};
        };
      private:
        std::mutex mutex;
        std::vector<Event> events; // Allocated only after the opt-in caller records an event.
        std::uint64_t sequence{};
      public:
        void Record(Event event) {
            std::scoped_lock lock{mutex};
            if (events.empty()) events.resize(Capacity);
            event.sequence=++sequence;
            events[(sequence-1)%Capacity]=event;
        }
        std::string Snapshot(std::uintptr_t space,std::uint64_t address,std::uint64_t bytes) {
            std::scoped_lock lock{mutex};
            std::ostringstream out;
            const auto first=sequence > Capacity ? sequence-Capacity+1 : 1;
            out << "transfer_history last_seq=" << sequence << " overwritten=" << first-1
                << " matching=conservative_iova_envelope command_completion=unverified"
                   " physical_aliases_and_remaps=unverified cpu_writes=not_observed\n";
            size_t matches{}, emitted{};
            for (auto serial=sequence; serial && serial>=first; --serial) {
                const auto &e=events[(serial-1)%Capacity];
                if (e.addressSpace!=space || !bytes || !e.bytes ||
                    !(e.destination<=address ? address-e.destination<e.bytes : e.destination-address<bytes)) continue;
                ++matches;
                if (emitted++>=128) continue;
                out << "transfer seq=" << e.sequence << " kind=" << e.kind
                    << " source=0x" << std::hex << e.source << " destination=0x" << e.destination
                    << std::dec << " envelope_bytes=" << e.bytes << " raw_words=";
                for (size_t i=0;i<std::min(e.wordCount,e.words.size());++i) out << std::hex << e.words[i] << ',';
                out << std::dec << '\n';
            }
            out << "transfer_matches=" << matches << " omitted=" << (matches>128 ? matches-128 : 0) << '\n';
            return out.str();
        }
    };
    inline SparkingTransferTrace &SparkingTransfers() {
        static SparkingTransferTrace transfers;
        return transfers;
    }

    // Why the actual cubemap upload did not leave a retained proof.
    // A missing proof alone must never be interpreted as evidence that
    // Vulkan or guest memory was valid or invalid.
    enum class SparkingUploadStatus : std::uint8_t {
        NeverObserved,
        SynchronizeEntered,
        NoStagingBuffer,
        UnsupportedTileMode,
        OutsideFocusedCubemap,
        GuestBytesUnavailable,
        GuestBytesOverLimit,
        LinearBytesOverLimit,
        UploadBytesOverLimit,
        BudgetRejected,
        ProofRetained,
        UploadRecorded,
    };

    constexpr const char *SparkingUploadStatusName(SparkingUploadStatus status) {
        switch (status) {
            case SparkingUploadStatus::NeverObserved: return "never_observed";
            case SparkingUploadStatus::SynchronizeEntered: return "synchronize_entered";
            case SparkingUploadStatus::NoStagingBuffer: return "no_staging_buffer";
            case SparkingUploadStatus::UnsupportedTileMode: return "unsupported_tile_mode";
            case SparkingUploadStatus::OutsideFocusedCubemap: return "outside_focused_cubemap";
            case SparkingUploadStatus::GuestBytesUnavailable: return "guest_bytes_unavailable";
            case SparkingUploadStatus::GuestBytesOverLimit: return "guest_bytes_over_limit";
            case SparkingUploadStatus::LinearBytesOverLimit: return "linear_bytes_over_limit";
            case SparkingUploadStatus::UploadBytesOverLimit: return "upload_bytes_over_limit";
            case SparkingUploadStatus::BudgetRejected: return "budget_rejected";
            case SparkingUploadStatus::ProofRetained: return "proof_retained";
            case SparkingUploadStatus::UploadRecorded: return "upload_recorded";
        }
        return "unknown";
    }

    // Match the captured first-draw HDR cubemap geometry, before capture is armed.
    constexpr bool IsSparkingCubeGeometry(size_t width, size_t height, size_t depth,
                                         size_t layers, size_t mips) {
        return width == 128 && height == 128 && depth == 1 &&
               layers == 6 && mips == 8;
    }

    struct SparkingUploadProof {
        std::shared_ptr<std::atomic<size_t>> budget;
        size_t charge{};
        std::vector<std::uint8_t> guest, linear, upload;
        std::shared_ptr<SparkingUploadProof> creation;
        std::uintptr_t addressSpace{};
        std::uint64_t iova{};
        bool ticRecorded{};
        std::string metadata;
        const void *stagingIdentity{};
        std::uint64_t writeSerial{}, backingGeneration{};
        std::atomic<bool> recorded{false};
        struct Slice { size_t mip, layer, guestOffset, guestBytes, linearOffset, linearBytes, hostOffset, hostBytes; };
        std::vector<Slice> slices;
        ~SparkingUploadProof() { if (budget) budget->fetch_sub(charge); }

        static std::uint32_t Crc(std::span<const std::uint8_t> bytes) {
            std::uint32_t crc{0xFFFFFFFF};
            for (auto byte : bytes) {
                crc ^= byte;
                for (unsigned bit{}; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
            return ~crc;
        }
        // Returns the common length for equal inputs; a shorter input differs at its end.
        static size_t FirstDifference(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
            return static_cast<size_t>(std::mismatch(a.begin(), a.end(), b.begin(), b.end()).first - a.begin());
        }
        bool Write(const std::filesystem::path &prefix, std::span<const std::uint8_t> readback,
                   const std::string &current) const {
            const auto blob{[&](const char *suffix, const auto &bytes) {
                std::ofstream out(prefix.string() + suffix, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                return static_cast<bool>(out);
            }};
            if (!blob("_guest.raw", guest) || !blob("_linear.raw", linear) || !blob("_upload.raw", upload))
                return false;
            std::ofstream out(prefix.string() + ".txt");
            if (creation) {
                if (!blob("_creation_guest.raw", creation->guest)) return false;
                out << creation->metadata
                    << "creation_guest_bytes=" << creation->guest.size()
                    << " creation_guest_crc32=" << Crc(creation->guest)
                    << " upload_guest_crc32=" << Crc(guest)
                    << " creation_upload_guest_equal=" << (creation->guest==guest)
                    << " creation_upload_first_difference=" << FirstDifference(creation->guest,guest) << '\n';
            } else out << "creation_proof=unavailable\n";
            out << metadata << current << "recorded=" << recorded.load(std::memory_order_relaxed) << " upload_write_serial=" << writeSerial
                << " upload_backing_generation=" << backingGeneration << '\n';
            for (const auto &s : slices) {
                if (s.guestOffset > guest.size() || s.guestBytes > guest.size() - s.guestOffset ||
                    s.linearOffset > linear.size() || s.linearBytes > linear.size() - s.linearOffset ||
                    s.hostOffset > upload.size() || s.hostBytes > upload.size() - s.hostOffset ||
                    s.hostOffset > readback.size() || s.hostBytes > readback.size() - s.hostOffset) {
                    out << "invalid_slice mip=" << s.mip << " face=" << s.layer << '\n';
                    continue;
                }
                const auto before{std::span(upload).subspan(s.hostOffset, s.hostBytes)};
                const auto after{readback.subspan(s.hostOffset, s.hostBytes)};
                out << "mip=" << s.mip << " face=" << s.layer
                    << " guest_offset=" << s.guestOffset << " guest_bytes=" << s.guestBytes
                    << " guest_crc32=" << Crc(std::span(guest).subspan(s.guestOffset, s.guestBytes))
                    << " linear_offset=" << s.linearOffset << " linear_bytes=" << s.linearBytes
                    << " linear_crc32=" << Crc(std::span(linear).subspan(s.linearOffset, s.linearBytes))
                    << " host_offset=" << s.hostOffset << " host_bytes=" << s.hostBytes
                    << " upload_crc32=" << Crc(before) << " readback_crc32=" << Crc(after)
                    << " equal=" << std::equal(before.begin(), before.end(), after.begin(), after.end())
                    << " first_difference=" << FirstDifference(before, after) << '\n';
            }
            return static_cast<bool>(out);
        }
    };

    class SparkingUploadProvenance {
        std::shared_ptr<std::atomic<size_t>> bytes{std::make_shared<std::atomic<size_t>>(0)};
      public:
        // Opt-in BEFORE loading the game, independent of the later six-frame capture.
        std::atomic<bool> enabled{false};
        static constexpr size_t MaxBytes{32ULL * 1024 * 1024};
        size_t UsedBytes() const { return bytes->load(std::memory_order_relaxed); }
        std::shared_ptr<SparkingUploadProof> CaptureCreation(std::span<const std::uint8_t> memory) {
            if (memory.empty() || memory.size()>4ULL*1024*1024) return {};
            auto proof=Reserve(memory.size());
            if (proof) proof->guest.assign(memory.begin(),memory.end());
            return proof;
        }
        std::shared_ptr<SparkingUploadProof> Reserve(size_t count) {
            if (!enabled.load(std::memory_order_relaxed) || !count || count > MaxBytes) return {};
            auto proof{std::make_shared<SparkingUploadProof>()};
            auto used{bytes->load()};
            do {
                if (used > MaxBytes - count) return {};
            } while (!bytes->compare_exchange_weak(used, used + count));
            proof->budget = bytes;
            proof->charge = count;
            return proof;
        }
    };
    inline SparkingUploadProvenance &SparkingUploads() {
        static SparkingUploadProvenance provenance;
        return provenance;
    }
}
