// SPDX-License-Identifier: MPL-2.0
// Host Vulkan integration test: run with VK_LAYER_KHRONOS_validation and an ICD.
#include <gpu/diagnostic/re5_scalar_copy.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
    unsigned errors{};
    VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++errors;
            std::fprintf(stderr, "%s\n", data->pMessage);
        }
        return VK_FALSE;
    }
}

int main() {
    vk::raii::Context context{vkGetInstanceProcAddr};
    const char *layer{"VK_LAYER_KHRONOS_validation"};
    const char *extension{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const vk::ValidationFeatureEnableEXT feature{vk::ValidationFeatureEnableEXT::eSynchronizationValidation};
    vk::ValidationFeaturesEXT features{.enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &feature};
    vk::ApplicationInfo app{.pApplicationName = "RE5 scalar readback test", .apiVersion = VK_API_VERSION_1_1};
    vk::raii::Instance instance{context, vk::InstanceCreateInfo{
        .pNext = &features, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension,
    }};
    vk::raii::DebugUtilsMessengerEXT debug{instance, vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
        .pfnUserCallback = Message,
    }};
    vk::raii::PhysicalDevices physicalDevices{instance};
    assert(!physicalDevices.empty());
    auto &physical{physicalDevices.front()};
    const auto families{physical.getQueueFamilyProperties()};
    std::uint32_t family{};
    while (family < families.size() && !(families[family].queueFlags & vk::QueueFlagBits::eGraphics))
        ++family;
    assert(family < families.size());
    const float priority{1};
    vk::DeviceQueueCreateInfo queueInfo{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    vk::raii::Device device{physical, vk::DeviceCreateInfo{.queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo}};
    vk::raii::Queue queue{device, family, 0};
    auto allocate{[&](const vk::raii::Buffer &buffer) {
        const auto requirements{buffer.getMemoryRequirements()};
        const auto properties{physical.getMemoryProperties()};
        std::uint32_t type{};
        while (type < properties.memoryTypeCount &&
            (!(requirements.memoryTypeBits & (1U << type)) ||
             !(properties.memoryTypes[type].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)))
            ++type;
        assert(type < properties.memoryTypeCount);
        return vk::raii::DeviceMemory{device, vk::MemoryAllocateInfo{
            .allocationSize = requirements.size, .memoryTypeIndex = type}};
    }};
    vk::raii::Buffer source{device, vk::BufferCreateInfo{.size = 1024,
        .usage = vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst}};
    auto sourceMemory{allocate(source)};
    source.bindMemory(*sourceMemory, 0);
    vk::raii::Buffer destination{device, vk::BufferCreateInfo{.size = 16,
        .usage = vk::BufferUsageFlagBits::eTransferDst}};
    auto destinationMemory{allocate(destination)};
    destination.bindMemory(*destinationMemory, 0);
    auto *mapped{static_cast<unsigned char *>(destinationMemory.mapMemory(0, VK_WHOLE_SIZE))};
    std::memset(mapped, 0xA5, 16);
    device.flushMappedMemoryRanges(vk::MappedMemoryRange{.memory = *destinationMemory, .offset = 0, .size = VK_WHOLE_SIZE});
    vk::raii::CommandPool pool{device, vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer, .queueFamilyIndex = family}};
    vk::raii::CommandBuffers commands{device, vk::CommandBufferAllocateInfo{
        .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}};
    auto &command{commands.front()};
    vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
    for (std::uint32_t expected : {0U, 0x3F800000U}) {
        command.begin(vk::CommandBufferBeginInfo{});
        using skyline::gpu::diagnostic::RecordScalarReadback;
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{}, 0, *destination));
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, 64, 452}, 448, {}));
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, 65, 452}, 448, *destination));
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, 64, 451}, 448, *destination));
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, 64, VK_WHOLE_SIZE}, 448, *destination));
        assert(!RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, UINT64_MAX - 2, 452}, 448, *destination));
        command.updateBuffer<std::uint32_t>(*source, 512, expected);
        const auto offset{RecordScalarReadback(command, vk::DescriptorBufferInfo{*source, 64, 452}, 448, *destination)};
        assert(offset && *offset == 512);
        const std::uint32_t later{0xDEADBEEF};
        command.updateBuffer<std::uint32_t>(*source, 512, later);
        command.end();
        vk::CommandBuffer handle{*command};
        queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &handle}, *fence);
        assert(device.waitForFences(*fence, true, UINT64_MAX) == vk::Result::eSuccess);
        device.invalidateMappedMemoryRanges(vk::MappedMemoryRange{.memory = *destinationMemory, .offset = 0, .size = VK_WHOLE_SIZE});
        std::uint32_t actual{};
        std::memcpy(&actual, mapped, 4);
        assert(actual == expected);
        for (unsigned i = 4; i < 16; ++i)
            assert(mapped[i] == 0xA5);
        device.resetFences(*fence);
        command.reset({});
    }
    destinationMemory.unmapMemory();
    assert(errors == 0);
    std::puts("Scalar Vulkan copy: zero/one values, exact offset, four bytes, later write; zero validation errors");
}
