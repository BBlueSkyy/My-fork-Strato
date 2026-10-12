// SPDX-License-Identifier: MPL-2.0
// Host integration check: requires a Vulkan ICD and VK_LAYER_KHRONOS_validation.
#include <gpu/texture/image_format_policy.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    unsigned validationErrors{};
    VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++validationErrors;
            std::fprintf(stderr, "%s\n", data->pMessage);
        }
        return VK_FALSE;
    }
    void Check(VkResult result) {
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan operation failed: %d\n", result);
            std::exit(2);
        }
    }
}

int main() {
    const char *layer{"VK_LAYER_KHRONOS_validation"};
    const char *extension{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledLayerCount = instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledLayerNames = &layer;
    instanceInfo.ppEnabledExtensionNames = &extension;
    VkInstance instance{};
    Check(vkCreateInstance(&instanceInfo, nullptr, &instance));
    auto createDebug{reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"))};
    auto destroyDebug{reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))};
    if (!createDebug || !destroyDebug) return 2;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debugInfo.pfnUserCallback = ValidationMessage;
    VkDebugUtilsMessengerEXT debug{};
    Check(createDebug(instance, &debugInfo, nullptr, &debug));

    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count) return 2;
    std::vector<VkPhysicalDevice> physicalDevices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data()));
    VkPhysicalDevice physical{physicalDevices.front()};
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    uint32_t family{};
    while (family < count && !queues[family].queueCount) ++family;
    if (family == count) return 2;
    float priority{1};
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device{};
    Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

    for (bool reverse : {false, true}) {
        auto native{reverse ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = static_cast<VkImageCreateFlags>(skyline::gpu::texture::GuestImageFormatFlags(native, true));
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = static_cast<VkFormat>(native);
        info.extent = {reverse ? 32U : 17U, 26, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physical, info.format, &formatProperties);
        if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            info.usage |= VK_IMAGE_USAGE_STORAGE_BIT; // Same feature-based usage as Texture.
        VkImage image{};
        Check(vkCreateImage(device, &info, nullptr, &image));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        uint32_t memoryType{};
        while (memoryType < memoryProperties.memoryTypeCount && !(requirements.memoryTypeBits & (1U << memoryType))) ++memoryType;
        if (memoryType == memoryProperties.memoryTypeCount) return 2;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        VkDeviceMemory memory{};
        Check(vkAllocateMemory(device, &allocation, nullptr, &memory));
        Check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = reverse ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        Check(vkCreateImageView(device, &viewInfo, nullptr, &view));
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    destroyDebug(instance, debug, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (validationErrors) return 1;
    std::puts("RGBA/BGRA image views: zero Vulkan validation errors");
}
