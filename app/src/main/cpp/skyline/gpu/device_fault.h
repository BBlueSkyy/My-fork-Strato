// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include "device_fault_compat.h"

namespace skyline::gpu {
    inline VkPhysicalDeviceFaultFeaturesEXT QueryDeviceFaultFeatures(bool advertised, VkPhysicalDevice physicalDevice, PFN_vkGetPhysicalDeviceFeatures2 getFeatures) {
        VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
        faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        if (advertised) {
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &faultFeatures;
            getFeatures(physicalDevice, &features);
            // Binary dump generation is independent and is deliberately left disabled.
            faultFeatures.deviceFaultVendorBinary = VK_FALSE;
        }
        return faultFeatures;
    }

    struct DeviceFaultReport {
        // Bound allocations and logging after a driver failure. Vendor binary collection is deliberately disabled.
        static constexpr uint32_t MaxEntries{64};
        VkResult result{VK_ERROR_UNKNOWN};
        bool truncated{};
        std::array<char, VK_MAX_DESCRIPTION_SIZE> description{};
        uint32_t addressCount{};
        uint32_t vendorCount{};
        std::array<VkDeviceFaultAddressInfoEXT, MaxEntries> addresses{};
        std::array<VkDeviceFaultVendorInfoEXT, MaxEntries> vendors{};
    };

    class DeviceFaultDiagnostics {
      private:
        std::atomic_flag captured{};

      public:
        /**
         * @brief Collect a bounded report once per device, only after VK_ERROR_DEVICE_LOST has been observed.
         * @param featureEnabled True only when VK_EXT_device_fault and its deviceFault feature were enabled at device creation.
         */
        std::optional<DeviceFaultReport> CaptureAfterDeviceLoss(bool featureEnabled, VkDevice device, PFN_vkGetDeviceFaultInfoEXT getFaultInfo) {
            if (!featureEnabled || captured.test_and_set(std::memory_order_relaxed))
                return std::nullopt;

            DeviceFaultReport report;
            if (!getFaultInfo) {
                report.result = VK_ERROR_EXTENSION_NOT_PRESENT;
                return report;
            }

            VkDeviceFaultCountsEXT counts{};
            counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
            report.result = getFaultInfo(device, &counts, nullptr);
            if (report.result != VK_SUCCESS)
                return report;

            report.truncated = counts.addressInfoCount > DeviceFaultReport::MaxEntries || counts.vendorInfoCount > DeviceFaultReport::MaxEntries;
            counts.addressInfoCount = std::min(counts.addressInfoCount, DeviceFaultReport::MaxEntries);
            counts.vendorInfoCount = std::min(counts.vendorInfoCount, DeviceFaultReport::MaxEntries);
            counts.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
            info.pAddressInfos = counts.addressInfoCount ? report.addresses.data() : nullptr;
            info.pVendorInfos = counts.vendorInfoCount ? report.vendors.data() : nullptr;
            report.result = getFaultInfo(device, &counts, &info);
            if (report.result == VK_SUCCESS || report.result == VK_INCOMPLETE) {
                std::copy(std::begin(info.description), std::end(info.description), report.description.begin());
                report.addressCount = std::min(counts.addressInfoCount, DeviceFaultReport::MaxEntries);
                report.vendorCount = std::min(counts.vendorInfoCount, DeviceFaultReport::MaxEntries);
                report.truncated |= report.result == VK_INCOMPLETE;
            }
            return report;
        }
    };
}
