// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <bitset>
#include <iostream>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <shader_compiler/profile.h>
namespace vk::raii {
struct Context;
struct PhysicalDevice {
    template<class... Features> vk::StructureChain<Features...> getFeatures2() const { return {}; }
    vk::FormatProperties getFormatProperties(vk::Format) const { return {}; }
    vk::PhysicalDeviceMemoryProperties2 getMemoryProperties2() const { return {}; }
};
}
struct adrenotools_gpu_mapping;
#define LOGW(...) ((void)0)
namespace skyline::util {
template<class T, class U> constexpr T AlignUp(T value, U align) { return (value + align - 1) & ~(static_cast<T>(align) - 1); }
constexpr uint64_t Hash(std::string_view s) {
    uint64_t h = 14695981039346656037ULL;
    for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    return h;
}
}
#include "traits.inc"
namespace skyline::gpu {
TraitManager::QuirkManager::QuirkManager(const vk::PhysicalDeviceProperties &, const vk::PhysicalDeviceDriverProperties &) {}
}
#include "constructor.inc"
using skyline::gpu::TraitManager;
namespace util = skyline::util;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
Shader::Profile profileFor(const TraitManager &traits) {
    struct Settings { const bool *disableSubgroupShuffle; } settings;
    const bool disabled{}; settings.disableSubgroupShuffle = &disabled;
    struct State { Settings *settings; } state{&settings};
    constexpr u32 TegraX1WarpSize = 32;
    Shader::Profile profile;
#include "profile.inc"
    return profile;
}
struct Range { u64 address, size, padding; };
Range rangeFor(const TraitManager &traits, u64 address, u32 size, bool known) {
    struct SSBO { u64 address; u32 size; } ssbo{address, size};
    struct Descriptor { bool has_size; } desc{known};
    struct GPU { const TraitManager &traits; } gpu{traits};
    struct Context { GPU &gpu; } ctx{gpu};
    std::optional<u64> accessSize{size};
#include "range.inc"
    return {address - padding, range, padding};
}
int main() {
    try {
        const vk::raii::PhysicalDevice physicalDevice;
#include "query.inc"
        static_assert(std::is_same_v<decltype(deviceFeatures2), TraitManager::DeviceFeatures2>);
        unsigned cases{};
        for (unsigned mask = 0; mask < 64; ++mask) {
            TraitManager::DeviceFeatures2 supported, enabled;
            supported.get<vk::PhysicalDeviceFeatures2>().features.shaderInt16 = bool(mask & 1);
            supported.get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray = bool(mask & 1);
            supported.get<vk::PhysicalDeviceShaderFloat16Int8Features>().shaderInt8 = bool(mask & 2);
            supported.get<vk::PhysicalDevice8BitStorageFeatures>().storageBuffer8BitAccess = bool(mask & 4);
            supported.get<vk::PhysicalDevice16BitStorageFeatures>().storageBuffer16BitAccess = bool(mask & 8);
            supported.get<vk::PhysicalDeviceFeatures2>().features.shaderStorageImageWriteWithoutFormat = bool(mask & 16);
            std::vector<vk::ExtensionProperties> extensions;
            auto add = [&](const char *name) {
                vk::ExtensionProperties e;
                std::copy(name, name + std::char_traits<char>::length(name) + 1, e.extensionName.begin());
                extensions.push_back(e);
            };
            add("VK_KHR_shader_float16_int8");
            if (mask & 32) add("VK_KHR_8bit_storage");
            std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> requested;
            TraitManager::DeviceProperties2 properties;
            properties.get<vk::PhysicalDeviceProperties2>().properties.apiVersion = VK_MAKE_VERSION(1, 3, 0);
            const unsigned hostAlignment = 1U << (mask % 9);
            properties.get<vk::PhysicalDeviceProperties2>().properties.limits.minStorageBufferOffsetAlignment = hostAlignment;
            const unsigned hostMaximum = (1U << 27) + (mask % 4);
            properties.get<vk::PhysicalDeviceProperties2>().properties.limits.maxStorageBufferRange = hostMaximum;
            vk::raii::PhysicalDevice device;
            TraitManager traits(supported, enabled, extensions, requested, properties, device);
            const auto profile = profileFor(traits);
            check(enabled.get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray == bool(mask & 1),
                  "supported cube image arrays were not enabled");
            check(traits.minimumStorageBufferAlignment == std::max(4U, hostAlignment),
                  "effective SSBO alignment must cover the enclosing CAS word");
            check(traits.maximumStorageBufferRange == (hostMaximum & ~3U), "maximum SSBO range leaves an incomplete CAS word");
            for (u64 address : {1ULL, 2ULL, 3ULL, 4ULL, 15ULL})
                for (u32 size : {1U, 2U, 3U, 4U, 5U, 7U})
                    for (bool known : {false, true}) {
                        const auto r = rangeFor(traits, address, size, known);
                        check(r.address % 4 == 0 && r.size % 4 == 0 && r.size >= r.padding + size,
                              "odd-address short SSBO does not map the whole CAS word");
                    }
            const bool storage8 = bool(mask & 4) && bool(mask & 32);
            check(traits.supportsStorage8 == storage8 && profile.support_storage8 == storage8,
                  "8-bit SSBO support must require enabled extension and feature");
            check(traits.supportsStorage16 == bool(mask & 8) && profile.support_storage16 == bool(mask & 8),
                  "16-bit SSBO support must reflect optional core1.1 feature");
            check(profile.support_int8 == bool(mask & 2) && profile.support_int16 == bool(mask & 1),
                  "arithmetic feature incorrectly coupled to storage");
            check(enabled.get<vk::PhysicalDevice16BitStorageFeatures>().storageBuffer16BitAccess == bool(mask & 8),
                  "advertised storage16 feature was not enabled");
            check(enabled.isLinked<vk::PhysicalDevice8BitStorageFeatures>() == bool(mask & 32),
                  "unsupported 8-bit storage pNext was not unlinked");
            if (mask & 32)
                check(enabled.get<vk::PhysicalDevice8BitStorageFeatures>().storageBuffer8BitAccess == storage8,
                      "advertised storage8 feature was not enabled");
            check(profile.support_typeless_image_writes == bool(mask & 16), "typeless writes lost enabled feature gate");
            ++cases;
        }
        std::cout << "PASS " << cases << " Vulkan extension/feature/profile combinations\n";
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
