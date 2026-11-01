// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.h>
#include <chrono>
static std::atomic<unsigned> validationErrors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::fprintf(stderr, "Vulkan: %s\n", data->pMessage);
    }
    return false;
}
static void VkCheck(VkResult result) {
    Check(result == VK_SUCCESS, "Vulkan operation failed");
}
struct Vulkan {
    bool externalDeviceInstanceOwnership{}; // Optional RAII owner outlives this fixture.
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkDebugUtilsMessengerEXT messenger{};
    uint32_t family{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    Vulkan() {
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{};
        features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = &sync;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pNext = &features;
        ci.pApplicationInfo = &app;
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = extensions;
        VkCheck(vkCreateInstance(&ci, nullptr, &instance));
        VkDebugUtilsMessengerCreateInfoEXT di{};
        di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        di.pfnUserCallback = Validation;
        VkCheck(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &di, nullptr,
                                                                                                                                        &messenger));
        uint32_t count{};
        VkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        Check(count, "no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        VkCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        physical = devices.front();
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (!(families.at(family).queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{};
        dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qi;
        VkCheck(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        VkCommandPoolCreateInfo pc{};
        pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pc.queueFamilyIndex = family;
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCheck(vkCreateCommandPool(device, &pc, nullptr, &pool));
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        std::printf("Native Vulkan device: %s\n", props.deviceName);
    }
    ~Vulkan() {
        vkDestroyCommandPool(device, pool, nullptr);
        if (!externalDeviceInstanceOwnership)
            vkDestroyDevice(device, nullptr);
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        if (!externalDeviceInstanceOwnership)
            vkDestroyInstance(instance, nullptr);
    }
    VkCommandBuffer Begin() {
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCheck(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VkCheck(vkBeginCommandBuffer(cmd, &bi));
        return cmd;
    }
    VkFence Submit(VkCommandBuffer cmd) {
        VkCheck(vkEndCommandBuffer(cmd));
        VkFence fence{};
        VkFenceCreateInfo fc{};
        fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkCheck(vkCreateFence(device, &fc, nullptr, &fence));
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkCheck(vkQueueSubmit(queue, 1, &si, fence));
        return fence;
    }
    void Retire(VkCommandBuffer cmd, VkFence fence) {
        VkCheck(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
};
struct NativeBuffer {
    Vulkan& vk;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    u8* data{};
    NativeBuffer(Vulkan& vk) : vk(vk) {
        VkBufferCreateInfo bc{};
        bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bc.size = Fixture::Size;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(vk.device, &bc, nullptr, &buffer));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        uint32_t type{};
        for (; type < vk.memoryProperties.memoryTypeCount; ++type)
            if ((req.memoryTypeBits & (1U << type)) &&
                ((vk.memoryProperties.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
                break;
        Check(type < vk.memoryProperties.memoryTypeCount, "no coherent host memory");
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory));
        VkCheck(vkBindBufferMemory(vk.device, buffer, memory, 0));
        void* mapped{};
        VkCheck(vkMapMemory(vk.device, memory, 0, Fixture::Size, 0, &mapped));
        data = static_cast<u8*>(mapped);
    }
    ~NativeBuffer() {
        vkUnmapMemory(vk.device, memory);
        vkDestroyBuffer(vk.device, buffer, nullptr);
        vkFreeMemory(vk.device, memory, nullptr);
    }
};
static void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
