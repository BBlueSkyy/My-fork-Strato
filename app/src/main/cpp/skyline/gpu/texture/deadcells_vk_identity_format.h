// SPDX-License-Identifier: MPL-2.0
// Probe-only, dependency-free metadata labels for native Vulkan images.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace skyline::gpu::deadcells_probe::vk_link {
    template <typename T>
    [[nodiscard]] std::uint64_t HandleBits(T handle) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T) <= sizeof(std::uint64_t));
        std::uint64_t value{};
        std::memcpy(&value, &handle, sizeof(T));
        return value;
    }

    [[nodiscard]] inline std::string Name(const char *role, std::uint64_t frame,
                                          std::uint64_t serial,
                                          std::uint64_t guestVa,
                                          std::uint64_t hostVa,
                                          std::uint32_t nvmapHandle) {
        char label[224]{};
        std::snprintf(label, sizeof(label),
                      "DC425 role=%s frame=%llu gpuSerial=%llu guest=0x%llX host=0x%llX nvmap=%u",
                      role,
                      static_cast<unsigned long long>(frame),
                      static_cast<unsigned long long>(serial),
                      static_cast<unsigned long long>(guestVa),
                      static_cast<unsigned long long>(hostVa), nvmapHandle);
        return {label};
    }
}
