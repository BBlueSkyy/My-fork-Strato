// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skyline::gpu {
    // One rolling candidate, never a growing shader archive. Only used when the
    // user explicitly enables compute pipeline diagnostics before launching.
    class ComputeDiagnosticStore {
        bool enabled;
        std::filesystem::path path;

        bool Write(std::string_view name, const char *data, std::size_t size) {
            std::ofstream file{path / name, std::ios::binary | std::ios::trunc};
            file.write(data, static_cast<std::streamsize>(size));
            file.close();
            return !file.fail();
        }

      public:
        static constexpr std::size_t MaxShaderBytes{2 * 1024 * 1024};
        static constexpr std::size_t MaxMetadataBytes{64 * 1024};

        ComputeDiagnosticStore(bool enabled, std::filesystem::path path)
            : enabled{enabled}, path{std::move(path)} {}

        bool Enabled() const { return enabled; }
        const std::filesystem::path &Path() const { return path; }

        bool Begin(std::string_view metadata, std::span<const std::uint8_t> guest,
                   std::span<const std::uint8_t> spirv, bool payloadComplete = true) noexcept {
            if (!enabled)
                return false;
            try {
                std::filesystem::create_directories(path);
                // Invalidate the previous candidate before replacing its payloads.
                for (const auto *name : {"metadata.txt", "result.txt", "guest.bin", "shader.spv"})
                    std::filesystem::remove(path / name);
                if (!payloadComplete) {
                    constexpr std::string_view omitted{"status=omitted_payload_unavailable\n"};
                    Write("metadata.txt", omitted.data(), omitted.size());
                    return false;
                }
                if (guest.size() > MaxShaderBytes || spirv.size() > MaxShaderBytes || metadata.size() > MaxMetadataBytes - 32) {
                    constexpr std::string_view omitted{"status=omitted_size_limit\n"};
                    Write("metadata.txt", omitted.data(), omitted.size());
                    return false;
                }
                if (!Write("guest.bin", reinterpret_cast<const char *>(guest.data()), guest.size()) ||
                    !Write("shader.spv", reinterpret_cast<const char *>(spirv.data()), spirv.size()))
                    return false;
                const std::string record{"status=pending\n" + std::string{metadata}};
                return Write("metadata.txt", record.data(), record.size());
            } catch (...) {
                // Diagnostic I/O must never replace a compiler/driver failure.
                return false;
            }
        }

        bool Finish(std::int32_t result, std::string_view resultName) noexcept {
            if (!enabled || resultName.size() > 256)
                return false;
            try {
                const std::string record{"vk_result=" + std::to_string(result) + "\nvk_result_name=" + std::string{resultName} + "\n"};
                return Write("result.txt", record.data(), record.size());
            } catch (...) {
                return false;
            }
        }
    };
}
