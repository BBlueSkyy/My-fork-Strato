// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
#include "gpu/device_fault.h"
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
struct adrenotools_gpu_mapping {};
namespace skyline {
    namespace util {
        constexpr size_t Hash(std::string_view text) {
            size_t hash{};
            for (char c : text) hash = hash * 31 + c;
            return hash;
        }
    }
    struct exception : std::runtime_error {
        template<typename... Args> exception(const char *message, Args &&...) : std::runtime_error(message) {}
    };
    struct AsyncLogger {
        enum class LogLevel { Info };
        static bool CheckLogLevel(LogLevel) { return false; }
    };
}
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)fmt::format(__VA_ARGS__))
#include "traits.inc"
namespace skyline::gpu {
    TraitManager::QuirkManager::QuirkManager(const vk::PhysicalDeviceProperties &, const vk::PhysicalDeviceDriverProperties &) {}
    void TraitManager::ApplyDriverPatches(const vk::raii::Context &, adrenotools_gpu_mapping *) {}
#include "negotiate.inc"
#include "create_device.inc"
    class GPU {
      public:
        DeviceFaultDiagnostics deviceFaultDiagnostics;
        TraitManager traits;
        vk::raii::Device vkDevice{nullptr};
        void ReportDeviceLoss(const char *) noexcept;
    };
#include "report.inc"
}

namespace skyline::gpu {
    struct FenceDevice {
        struct Dispatcher {} dispatcher;
        std::vector<vk::Result> results;
        unsigned calls{};
        FenceDevice &operator*() { return *this; }
        const Dispatcher *getDispatcher() { return &dispatcher; }
        vk::Result waitForFences(u32, vk::Fence *, bool, u64, const Dispatcher &) { return results.at(calls++); }
    };
    struct TestFenceCycle {
        std::atomic_flag signalled{};
        std::mutex mutex; std::shared_mutex chainMutex;
        std::condition_variable submitCondition;
        bool submitted{true};
        vk::Fence fence;
        FenceDevice device;
        struct Chains { template<typename F> void Iterate(F) {} } chainedCycles;
        std::shared_ptr<TestFenceCycle> semaphoreUnsignalCycle;
        void DestroyDependencies() {}
#include "wait.inc"
    };
}

static unsigned featureQueries{};
static VkBool32 supportedFault{};
static VKAPI_ATTR void VKAPI_CALL GetFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *features) {
    ++featureQueries;
    assert(features->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    auto &fault = *static_cast<VkPhysicalDeviceFaultFeaturesEXT *>(features->pNext);
    assert(fault.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
    assert(!fault.pNext);
    fault.deviceFault = supportedFault;
    fault.deviceFaultVendorBinary = VK_TRUE;
}
int main() {
    using namespace skyline::gpu;
    TestFenceCycle lost;
    lost.device.results = {vk::Result::eErrorDeviceLost};
    bool typedDeviceLoss{};
    try { lost.Wait(); } catch (const vk::DeviceLostError &) { typedDeviceLoss = true; } catch (...) {}
    assert(typedDeviceLoss && lost.device.calls == 1);
    TestFenceCycle retry;
    retry.device.results = {vk::Result::eTimeout, vk::Result::eErrorInitializationFailed, vk::Result::eSuccess};
    retry.Wait();
    assert(retry.device.calls == 3);

    auto fault = QueryDeviceFaultFeatures(false, {}, GetFeatures);
    assert(featureQueries == 0 && !fault.deviceFault && !fault.deviceFaultVendorBinary);
    fault = QueryDeviceFaultFeatures(true, {}, GetFeatures);
    assert(featureQueries == 1 && !fault.deviceFault && !fault.deviceFaultVendorBinary);
    supportedFault = VK_TRUE;
    fault = QueryDeviceFaultFeatures(true, {}, GetFeatures);
    assert(featureQueries == 2 && fault.deviceFault && !fault.deviceFaultVendorBinary);

    for (bool advertised : {false, true}) for (bool geometry : {false, true}) {
        TraitManager::DeviceFeatures2 features, enabled;
        features.get<vk::PhysicalDeviceFeatures2>().features.geometryShader = geometry;
        std::vector<vk::ExtensionProperties> extensions;
        if (advertised) {
            extensions.emplace_back();
            std::strcpy(extensions.back().extensionName.data(), "VK_NV_geometry_shader_passthrough");
        }
        std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> enabledExtensions;
        TraitManager::DeviceProperties2 properties;
        vk::raii::PhysicalDevice physicalDevice{nullptr};
        TraitManager traits(features, enabled, extensions, enabledExtensions, properties, physicalDevice);
        assert(traits.supportsGeometryShaders == geometry);
        assert(traits.supportsGeometryShaderPassthrough == (advertised && geometry));
        assert(enabled.get<vk::PhysicalDeviceFeatures2>().features.geometryShader == geometry);
        assert(enabledExtensions.size() == (advertised && geometry ? 1 : 0));
        if (!enabledExtensions.empty())
            assert(std::string_view(enabledExtensions.front().data()) == "VK_NV_geometry_shader_passthrough");
    }
}
