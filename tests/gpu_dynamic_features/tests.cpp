struct ComputeEnvironmentGate {
    bool supportsDynamicTextureHandles;
#include "environment.inc"
};
int main() {
    try {
        const vk::raii::PhysicalDevice physicalDevice;
#include "query.inc"
        static_assert(std::is_same_v<decltype(deviceFeatures2), TraitManager::DeviceFeatures2>);
        unsigned cases{};
        for (const auto appVersion : {VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_MAKE_VERSION(1, 3, 0)})
            for (const auto deviceVersion : {VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_MAKE_VERSION(1, 3, 0)})
                for (unsigned mask{}; mask < 32; ++mask) {
                    TraitManager::DeviceFeatures2 supported, enabled;
                    auto &supportedCore = supported.get<vk::PhysicalDeviceFeatures2>().features;
                    auto &supportedIndexing = supported.get<vk::PhysicalDeviceDescriptorIndexingFeatures>();
                    supportedCore.shaderSampledImageArrayDynamicIndexing = bool(mask & 1);
                    supportedIndexing.runtimeDescriptorArray = bool(mask & 2);
                    supportedIndexing.shaderSampledImageArrayNonUniformIndexing = bool(mask & 4);
                    supportedIndexing.descriptorBindingPartiallyBound = bool(mask & 8);
                    // These optional features must never be requested merely
                    // because the host advertises them.
                    supportedIndexing.descriptorBindingSampledImageUpdateAfterBind = true;
                    supportedIndexing.descriptorBindingUpdateUnusedWhilePending = true;
                    supportedIndexing.descriptorBindingVariableDescriptorCount = true;
                    std::vector<vk::ExtensionProperties> extensions;
                    if (mask & 16) {
                        vk::ExtensionProperties ext;
                        constexpr auto name = "VK_EXT_descriptor_indexing";
                        std::copy_n(name, std::char_traits<char>::length(name) + 1, ext.extensionName.begin());
                        extensions.push_back(ext);
                    }
                    std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> requested;
                    TraitManager::DeviceProperties2 properties;
                    auto &limits = properties.get().properties.limits;
                    properties.get().properties.apiVersion = deviceVersion;
                    limits.maxPerStageDescriptorSampledImages = 17;
                    limits.maxDescriptorSetSampledImages = 19;
                    limits.maxPerStageDescriptorSamplers = 23;
                    limits.maxDescriptorSetSamplers = 29;
                    limits.maxPerStageResources = 31;
                    limits.maxBoundDescriptorSets = 4;
                    limits.maxImageDimension2D = 8192;
                    TraitManager traits(supported, enabled, extensions, requested, properties, physicalDevice, appVersion);
                    const bool available = bool(mask & 16) || std::min(appVersion, deviceVersion) >= VK_API_VERSION_1_2;
                    const bool expected = available && (mask & 15) == 15;
                    check(traits.supportsDynamicTextureHandles == expected, "dynamic feature gate differs from enabled Vulkan features");
                    check(profileFor(traits).support_dynamic_texture_handles == expected, "SPIR-V profile gate differs from enabled Vulkan features");
                    check(ComputeEnvironmentGate{traits.supportsDynamicTextureHandles}.SupportsDynamicTextureHandles() == expected,
                          "compute translation gate differs from enabled Vulkan features");
                    check(enabled.isLinked<vk::PhysicalDeviceDescriptorIndexingFeatures>() == available,
                          "unsupported descriptor indexing feature struct remains linked");
                    const auto &enabledIndexing = enabled.get<vk::PhysicalDeviceDescriptorIndexingFeatures>();
                    check(bool(enabled.get<vk::PhysicalDeviceFeatures2>().features.shaderSampledImageArrayDynamicIndexing) == expected,
                          "core dynamic sampled image indexing was not enabled atomically");
                    check(bool(enabledIndexing.runtimeDescriptorArray) == expected &&
                          bool(enabledIndexing.shaderSampledImageArrayNonUniformIndexing) == expected &&
                          bool(enabledIndexing.descriptorBindingPartiallyBound) == expected,
                          "descriptor indexing capabilities were not enabled atomically");
                    check(!enabledIndexing.descriptorBindingSampledImageUpdateAfterBind &&
                          !enabledIndexing.descriptorBindingUpdateUnusedWhilePending && !enabledIndexing.descriptorBindingVariableDescriptorCount,
                          "unneeded descriptor indexing features were requested");
                    check(requested.size() == bool(mask & 16), "extension negotiation lost the application API requirement");
                    const auto &actual = traits.dynamicDescriptorLimits;
                    check(actual.maxPerStageDescriptorSampledImages == 17 && actual.maxDescriptorSetSampledImages == 19 &&
                          actual.maxPerStageDescriptorSamplers == 23 && actual.maxDescriptorSetSamplers == 29 &&
                          actual.maxPerStageResources == 31 && actual.maxBoundDescriptorSets == 4 && actual.maxImageDimension2D == 8192,
                          "dynamic descriptor validation limits are not the actual device limits");
                    ++cases;
                }
        std::cout << "PASS " << cases << " production dynamic descriptor feature/profile combinations\n";
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
