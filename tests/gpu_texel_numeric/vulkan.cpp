// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static unsigned errors{}, numericErrors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++errors; if (std::strstr(data->pMessage, "format-07753")) ++numericErrors; std::fprintf(stderr, "%s\n", data->pMessage); }
    return VK_FALSE;
}
static void Check(VkResult result) { if (result != VK_SUCCESS) { std::fprintf(stderr, "Vulkan failed: %d\n", result); std::exit(2); } }
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    const char *layer = "VK_LAYER_KHRONOS_validation", *debugExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT; debug.pfnUserCallback = Message;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledLayerCount = 1; instanceInfo.ppEnabledLayerNames = &layer;
    instanceInfo.enabledExtensionCount = 1; instanceInfo.ppEnabledExtensionNames = &debugExt; instanceInfo.pNext = &debug;
    VkInstance instance; Check(vkCreateInstance(&instanceInfo, nullptr, &instance));
    auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (!createDebug || !destroyDebug) return 2;
    VkDebugUtilsMessengerEXT messenger; Check(createDebug(instance, &debug, nullptr, &messenger));
    uint32_t count{}; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr)); if (!count) return 2;
    std::vector<VkPhysicalDevice> devices(count); Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = devices[0];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{}; while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family; if (family == count) return 2;
    float priority = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled; vkGetPhysicalDeviceFeatures(physical, &enabled);
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pEnabledFeatures = &enabled;
    VkDevice device; Check(vkCreateDevice(physical, &di, nullptr, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 256; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    VkBuffer buffer; Check(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties; vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    uint32_t memoryType{}; constexpr VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (memoryType < memoryProperties.memoryTypeCount && (!(requirements.memoryTypeBits & (1U << memoryType)) || (memoryProperties.memoryTypes[memoryType].propertyFlags & needed) != needed)) ++memoryType;
    if (memoryType == memoryProperties.memoryTypeCount) return 2;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory memory; Check(vkAllocateMemory(device, &allocation, nullptr, &memory)); Check(vkBindBufferMemory(device, buffer, memory, 0));
    void *mapped; Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = family;
    VkCommandPool pool; Check(vkCreateCommandPool(device, &cpi, nullptr, &pool));
    const bool negative = argc == 3;
    for (unsigned i = 0; i < (negative ? 1u : 3u); ++i) {
        const unsigned beforeErrors = errors;
        const VkFormat formats[3][3] = {
            {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT},
            {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT},
            {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT}};
        const uint32_t floatBits[4]{0x3f800001, 0xbf123456, 0x4a800001, 0x00000000};
        const uint32_t unsignedBits[4]{0xf1234567, 0xffffffff, 0x80000000, 0x00000001};
        const int32_t signedBits[4]{-1, INT32_MIN, -12345, 12345};
        VkBufferView views[3];
        for (unsigned n = 0; n < 3; ++n) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical, formats[i][n], &fp);
            if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) return 2;
            VkBufferViewCreateInfo vi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
            vi.buffer = buffer; vi.format = formats[i][n]; vi.offset = 64 * (n+1); vi.range = i == 2 ? (n == 0 ? 8 : 4) : n == 0 ? 16 : i == 0 ? 2 : 4;
            Check(vkCreateBufferView(device, &vi, nullptr, &views[n]));
        }
        VkDescriptorSetLayoutBinding bindings[4]{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        for (unsigned n = 1; n < 4; ++n) bindings[n] = {n, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 4; li.pBindings = bindings;
        VkDescriptorSetLayout layout; Check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
        VkPipelineLayout pipelineLayout; Check(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout));
        std::ifstream file(std::string(argv[1]) + "/mixed.spv", std::ios::binary | std::ios::ate);
        if (!file) return 2;
        auto size = static_cast<size_t>(file.tellg());
        if (size % 4) return 2;
        std::vector<uint32_t> code(size / 4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = size; smi.pCode = code.data();
        VkShaderModule module; Check(vkCreateShaderModule(device, &smi, nullptr, &module));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = module; pci.stage.pName = "main"; pci.layout = pipelineLayout;
        VkPipeline pipeline; Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 3}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
        VkDescriptorPool descriptorPool; Check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptorPool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
        VkDescriptorSet set; Check(vkAllocateDescriptorSets(device, &dai, &set));
        VkDescriptorBufferInfo output{buffer, 0, 48};
        VkWriteDescriptorSet writes[4]{};
        for (unsigned n = 0; n < 4; ++n) {
            writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[n].dstSet = set;
            writes[n].dstBinding = n; writes[n].descriptorCount = 1; writes[n].descriptorType = bindings[n].descriptorType;
            if (n == 0) writes[n].pBufferInfo = &output; else writes[n].pTexelBufferView = &views[n-1];
        }
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        std::memset(mapped, 0xCD, 256);
        auto bytes = static_cast<unsigned char*>(mapped);
        if (i < 2) {
            std::memcpy(bytes+64, floatBits, 16);
            if (i == 0) {
                const uint16_t u = 0xf123; const int16_t v = -12345;
                std::memcpy(bytes+128, &u, 2); std::memcpy(bytes+192, &v, 2);
            } else {
                std::memcpy(bytes+128, unsignedBits, 4); std::memcpy(bytes+192, signedBits, 4);
            }
        } else {
            const uint16_t unorm[4]{65535, 0, 65535, 0};
            const uint8_t u[4]{0xff, 0x80, 1, 0}; const int8_t v[4]{-128, -1, 0, 127};
            std::memcpy(bytes+64, unorm, 8); std::memcpy(bytes+128, u, 4); std::memcpy(bytes+192, v, 4);
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer command; Check(vkAllocateCommandBuffers(device, &cai, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Check(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, 1, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(command));
        if (negative) {
            // Validate the illegal pairing, but never submit its undefined reads.
            if (numericErrors != 2 || errors != numericErrors) return 1;
            std::printf("PASS negative control: two format-07753 errors, command buffer not submitted\n");
        } else {
            if (errors != beforeErrors) return 1;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; Check(vkCreateFence(device, &fi, nullptr, &fence));
        Check(vkQueueSubmit(queue, 1, &submit, fence)); Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        const auto *values = static_cast<const uint32_t*>(mapped);
        uint32_t expected[12]{};
        if (i < 2) {
            std::memcpy(expected, floatBits, 16);
            expected[4] = i == 0 ? 0xf123 : unsignedBits[0]; expected[7] = 1;
            expected[8] = i == 0 ? static_cast<uint32_t>(-12345) : static_cast<uint32_t>(signedBits[0]); expected[11] = 1;
        } else {
            const uint32_t e[12]{0x3f800000, 0, 0x3f800000, 0, 255, 128, 1, 0, 0xffffff80, 0xffffffff, 0, 127};
            std::memcpy(expected, e, sizeof(e));
        }
        for (unsigned n = 0; n < 12; ++n) if (values[n] != expected[n]) { std::fprintf(stderr, "case %u output %u = %X expected %X\n", i, n, values[n], expected[n]); return 1; }
        vkDestroyFence(device, fence, nullptr);
        }
        vkFreeCommandBuffers(device, pool, 1, &command);
        for (auto view : views) vkDestroyBufferView(device, view, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr); vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr); vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
    vkDestroyCommandPool(device, pool, nullptr);
    vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr); destroyDebug(instance, messenger, nullptr); vkDestroyInstance(instance, nullptr);
    if (!negative) std::printf("PASS texel numeric types: 3 format groups, 36 exact output words, %u validation errors\n", errors);
    return negative ? numericErrors == 2 ? 0 : 1 : errors ? 1 : 0;
}
