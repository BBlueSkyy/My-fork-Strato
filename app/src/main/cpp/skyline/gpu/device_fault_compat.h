// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <vulkan/vulkan.h>

#ifndef VK_EXT_device_fault
// VK_EXT_device_fault ABI definitions used below, from the Khronos Vulkan headers.
// Keep the existing Vulkan-Hpp version pinned; it predates this optional extension.
inline constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT{static_cast<VkStructureType>(1000341000)};
inline constexpr VkStructureType VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT{static_cast<VkStructureType>(1000341001)};
inline constexpr VkStructureType VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT{static_cast<VkStructureType>(1000341002)};

enum VkDeviceFaultAddressTypeEXT {
    VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT = 0,
    VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT = 1,
    VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT = 2,
    VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT = 3,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT = 4,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT = 5,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT = 6,
    VK_DEVICE_FAULT_ADDRESS_TYPE_MAX_ENUM_EXT = 0x7FFFFFFF
};

struct VkPhysicalDeviceFaultFeaturesEXT {
    VkStructureType sType;
    void *pNext;
    VkBool32 deviceFault;
    VkBool32 deviceFaultVendorBinary;
};

struct VkDeviceFaultCountsEXT {
    VkStructureType sType;
    void *pNext;
    uint32_t addressInfoCount;
    uint32_t vendorInfoCount;
    VkDeviceSize vendorBinarySize;
};

struct VkDeviceFaultAddressInfoEXT {
    VkDeviceFaultAddressTypeEXT addressType;
    VkDeviceAddress reportedAddress;
    VkDeviceSize addressPrecision;
};

struct VkDeviceFaultVendorInfoEXT {
    char description[VK_MAX_DESCRIPTION_SIZE];
    uint64_t vendorFaultCode;
    uint64_t vendorFaultData;
};

struct VkDeviceFaultInfoEXT {
    VkStructureType sType;
    void *pNext;
    char description[VK_MAX_DESCRIPTION_SIZE];
    VkDeviceFaultAddressInfoEXT *pAddressInfos;
    VkDeviceFaultVendorInfoEXT *pVendorInfos;
    void *pVendorBinaryData;
};

using PFN_vkGetDeviceFaultInfoEXT = VkResult (VKAPI_PTR *)(VkDevice device, VkDeviceFaultCountsEXT *pFaultCounts, VkDeviceFaultInfoEXT *pFaultInfo);
#endif
