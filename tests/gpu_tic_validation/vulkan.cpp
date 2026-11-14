// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
static unsigned errors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++errors; std::fprintf(stderr, "%s\n", data->pMessage); }
    return VK_FALSE;
}
static void Check(VkResult result) { if (result != VK_SUCCESS) { std::fprintf(stderr, "Vulkan failed: %d\n", result); std::exit(2); } }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
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
    VkPhysicalDeviceRobustness2FeaturesEXT robustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &robustness;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!robustness.nullDescriptor || !features.features.imageCubeArray) { std::fprintf(stderr, "Required features missing\n"); return 2; }
    robustness.robustBufferAccess2 = robustness.robustImageAccess2 = VK_FALSE; robustness.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceFeatures enabled{}; enabled.imageCubeArray = VK_TRUE;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{}; while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family; if (family == count) return 2;
    float priority = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    const char *robustnessExt = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &robustnessExt; di.pNext = &robustness; di.pEnabledFeatures = &enabled;
    VkDevice device; Check(vkCreateDevice(physical, &di, nullptr, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 32; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer; Check(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties; vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    uint32_t memoryType{}; constexpr VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (memoryType < memoryProperties.memoryTypeCount && (!(requirements.memoryTypeBits & (1U << memoryType)) || (memoryProperties.memoryTypes[memoryType].propertyFlags & needed) != needed)) ++memoryType;
    if (memoryType == memoryProperties.memoryTypeCount) return 2;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory memory; Check(vkAllocateMemory(device, &allocation, nullptr, &memory)); Check(vkBindBufferMemory(device, buffer, memory, 0));
    void *mapped; Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.maxLod = 1;
    VkSampler sampler; Check(vkCreateSampler(device, &si, nullptr, &sampler));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex = family;
    VkCommandPool pool; Check(vkCreateCommandPool(device, &cpi, nullptr, &pool));
    for (unsigned i = 0; i < 8; ++i) {
        bool storage = i == 7;
        VkDescriptorSetLayoutBinding bindings[2]{{0, storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = bindings;
        VkDescriptorSetLayout layout; Check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
        VkPipelineLayout pipelineLayout; Check(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout));
        std::ifstream file(std::string(argv[1]) + "/" + std::to_string(i) + ".spv", std::ios::binary | std::ios::ate);
        if (!file) return 2;
        auto size = static_cast<size_t>(file.tellg());
        if (size % 4) return 2;
        std::vector<uint32_t> code(size / 4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = size; smi.pCode = code.data();
        VkShaderModule module; Check(vkCreateShaderModule(device, &smi, nullptr, &module));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = module; pci.stage.pName = "main"; pci.layout = pipelineLayout;
        VkPipeline pipeline; Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
        VkDescriptorPoolSize sizes[2]{{bindings[0].descriptorType, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
        VkDescriptorPool descriptorPool; Check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptorPool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
        VkDescriptorSet set; Check(vkAllocateDescriptorSets(device, &dai, &set));
        VkDescriptorImageInfo image{storage ? VK_NULL_HANDLE : sampler, VK_NULL_HANDLE, storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo output{buffer, 0, 32};
        VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1; writes[0].descriptorType = bindings[0].descriptorType; writes[0].pImageInfo = &image;
        writes[1].dstSet = set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &output;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        std::memset(mapped, 0xCD, 32);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer command; Check(vkAllocateCommandBuffers(device, &cai, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Check(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, 1, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; Check(vkCreateFence(device, &fi, nullptr, &fence));
        Check(vkQueueSubmit(queue, 1, &submit, fence)); Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        const auto *values = static_cast<const uint32_t*>(mapped);
        for (unsigned n = 0; n < 8; ++n) if (values[n] != 0) { std::fprintf(stderr, "case %u output %u = %X\n", i, n, values[n]); return 1; }
        vkDestroyFence(device, fence, nullptr); vkFreeCommandBuffers(device, pool, 1, &command);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr); vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr); vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
    vkDestroyCommandPool(device, pool, nullptr); vkDestroySampler(device, sampler, nullptr);
    vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr); destroyDebug(instance, messenger, nullptr); vkDestroyInstance(instance, nullptr);
    std::printf("Vulkan nullDescriptor: 7 sampled dimensions + storage write/read, 64 zero output words, %u validation errors\n", errors);
    return errors ? 1 : 0;
}
