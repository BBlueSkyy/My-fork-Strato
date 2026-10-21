// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace skyline::gpu::diagnostic {
    // Why the actual cubemap upload did not leave a retained proof.
    // A missing proof alone must never be interpreted as evidence that
    // Vulkan or guest memory was valid or invalid.
    enum class UploadRetentionStatus : std::uint8_t {
        NeverObserved,
        SynchronizeEntered,
        NoStagingBuffer,
        UnsupportedTileMode,
        GuestBytesOverLimit,
        LinearBytesOverLimit,
        UploadBytesOverLimit,
        BudgetRejected,
        ProofRetained,
        UploadRecorded,
    };

    constexpr const char *UploadRetentionStatusName(UploadRetentionStatus status) {
        switch (status) {
            case UploadRetentionStatus::NeverObserved: return "never_observed";
            case UploadRetentionStatus::SynchronizeEntered: return "synchronize_entered";
            case UploadRetentionStatus::NoStagingBuffer: return "no_staging_buffer";
            case UploadRetentionStatus::UnsupportedTileMode: return "unsupported_tile_mode";
            case UploadRetentionStatus::GuestBytesOverLimit: return "guest_bytes_over_limit";
            case UploadRetentionStatus::LinearBytesOverLimit: return "linear_bytes_over_limit";
            case UploadRetentionStatus::UploadBytesOverLimit: return "upload_bytes_over_limit";
            case UploadRetentionStatus::BudgetRejected: return "budget_rejected";
            case UploadRetentionStatus::ProofRetained: return "proof_retained";
            case UploadRetentionStatus::UploadRecorded: return "upload_recorded";
        }
        return "unknown";
    }

    struct UploadProof {
        std::shared_ptr<std::atomic<size_t>> budget;
        size_t charge{};
        std::vector<std::uint8_t> guest, linear, upload;
        std::string metadata;
        const void *stagingIdentity{};
        std::uint64_t writeSerial{}, backingGeneration{};
        bool recorded{};
        struct Slice { size_t mip, layer, guestOffset, guestBytes, linearOffset, linearBytes, hostOffset, hostBytes; };
        std::vector<Slice> slices;
        ~UploadProof() { if (budget) budget->fetch_sub(charge); }

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
            out << metadata << current << "recorded=" << recorded << " upload_write_serial=" << writeSerial
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

    class Ue4UploadProvenance {
        std::shared_ptr<std::atomic<size_t>> bytes{std::make_shared<std::atomic<size_t>>(0)};
      public:
        // Opt-in BEFORE loading the game, independent of the later six-frame capture.
        std::atomic<bool> enabled{false};
        static constexpr size_t MaxBytes{16ULL * 1024 * 1024};
        size_t UsedBytes() const { return bytes->load(std::memory_order_relaxed); }
        std::shared_ptr<UploadProof> Reserve(size_t count) {
            if (!enabled.load(std::memory_order_relaxed) || !count || count > MaxBytes) return {};
            auto proof{std::make_shared<UploadProof>()};
            auto used{bytes->load()};
            do {
                if (used > MaxBytes - count) return {};
            } while (!bytes->compare_exchange_weak(used, used + count));
            proof->budget = bytes;
            proof->charge = count;
            return proof;
        }
    };
    inline Ue4UploadProvenance &Ue4Uploads() {
        static Ue4UploadProvenance provenance;
        return provenance;
    }
}
