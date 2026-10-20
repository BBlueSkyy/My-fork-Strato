// SPDX-License-Identifier: MPL-2.0
// Run translated shader modules on a headless Vulkan graphics queue.
#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(call) do { const VkResult result = (call); if (result != VK_SUCCESS) \
    throw std::runtime_error(std::string(#call) + ": " + std::to_string(result)); } while (0)
static unsigned errors{};
static VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++errors;
        std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
struct Harness {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkQueryPool queries{};
    std::vector<VkShaderModule> modules;
    ~Harness() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyQueryPool(device, queries, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            vkDestroyRenderPass(device, pass, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) ((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
            "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    VkShaderModule Shader(const char *path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error(std::string("cannot open shader: ") + path);
        auto size = stream.tellg();
        if (size <= 0 || size % 4) throw std::runtime_error("bad SPIR-V size");
        std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
        stream.seekg(0);
        stream.read(reinterpret_cast<char *>(words.data()), size);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<size_t>(size);
        info.pCode = words.data();
        VkShaderModule module;
        CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
        modules.push_back(module);
        return module;
    }
    void Init() {
        const char *layer = "VK_LAYER_KHRONOS_validation";
        const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "strato-geometry-regressions";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = &layer;
        create.enabledExtensionCount = 1;
        create.ppEnabledExtensionNames = &extension;
        CHECK(vkCreateInstance(&create, nullptr, &instance));
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback = Debug;
        CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
            "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger));
        uint32_t count{};
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical{};
        uint32_t family{};
        VkPhysicalDeviceFeatures supported{};
        for (auto candidate : devices) {
            vkGetPhysicalDeviceFeatures(candidate, &supported);
            if (!supported.geometryShader || !supported.occlusionQueryPrecise) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical = candidate;
                family = i;
                break;
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("no Vulkan device with geometry and precise queries");
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("device=%s\n", properties.deviceName);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        enabled.geometryShader = VK_TRUE;
        enabled.occlusionQueryPrecise = VK_TRUE;
        enabled.shaderClipDistance = supported.shaderClipDistance;
        enabled.shaderCullDistance = supported.shaderCullDistance;
        enabled.shaderTessellationAndGeometryPointSize = supported.shaderTessellationAndGeometryPointSize;
        enabled.multiViewport = supported.multiViewport;
        // The production backend declares ImageGatherExtended even for fixtures
        // without texture instructions. Match Strato's supported feature enablement.
        enabled.shaderImageGatherExtended = supported.shaderImageGatherExtended;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &enabled;
        CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        CHECK(vkCreateRenderPass(device, &passInfo, nullptr, &pass));
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.width = framebufferInfo.height = 32;
        framebufferInfo.layers = 1;
        CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer));
        VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        queryInfo.queryCount = 1;
        CHECK(vkCreateQueryPool(device, &queryInfo, nullptr, &queries));
    }
    void Pipeline(const char *vertexPath, const char *fragmentPath, const char *geometryPath, VkPrimitiveTopology topology) {
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        auto stage = [&](const char *path, VkShaderStageFlagBits kind) {
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = kind;
            info.module = Shader(path);
            info.pName = "main";
            stages.push_back(info);
        };
        stage(vertexPath, VK_SHADER_STAGE_VERTEX_BIT);
        if (std::strcmp(geometryPath, "-")) stage(geometryPath, VK_SHADER_STAGE_GEOMETRY_BIT);
        stage(fragmentPath, VK_SHADER_STAGE_FRAGMENT_BIT);
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = topology;
        VkViewport viewport{0, 0, 32, 32, 0, 1};
        VkRect2D scissor{{0, 0}, {32, 32}};
        VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        view.viewportCount = view.scissorCount = 1;
        view.pViewports = &viewport;
        view.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &view;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.layout = layout;
        info.renderPass = pass;
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    }
    uint64_t Draw(uint32_t vertices) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command;
        CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdResetQueryPool(command, queries, 0, 1);
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent = {32, 32};
        vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const std::array<uint32_t, 32> constants{};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, constants.data());
        vkCmdBeginQuery(command, queries, 0, VK_QUERY_CONTROL_PRECISE_BIT);
        vkCmdDraw(command, vertices, 1, 0, 0);
        vkCmdEndQuery(command, queries, 0);
        vkCmdEndRenderPass(command);
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        uint64_t samples{};
        CHECK(vkGetQueryPoolResults(device, queries, 0, 1, sizeof(samples), &samples, sizeof(samples),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        vkFreeCommandBuffers(device, pool, 1, &command);
        return samples;
    }
};
int main(int argc, char **argv) {
    try {
        if (argc != 6) throw std::runtime_error("usage: vulkan VS.spv FS.spv GS.spv|- points|lines|triangles|lines-adj|triangles-adj vertex-count");
        VkPrimitiveTopology topology;
        const std::string name = argv[4];
        if (name == "points") topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        else if (name == "lines") topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        else if (name == "triangles") topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        else if (name == "lines-adj") topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        else if (name == "triangles-adj") topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        else throw std::runtime_error("unknown topology");
        Harness harness;
        harness.Init();
        harness.Pipeline(argv[1], argv[2], argv[3], topology);
        auto start = std::chrono::steady_clock::now();
        const uint64_t samples = harness.Draw(static_cast<uint32_t>(std::stoul(argv[5])));
        const uint64_t repeat = harness.Draw(static_cast<uint32_t>(std::stoul(argv[5])));
        if (samples != repeat) throw std::runtime_error("repeated draw changed coverage");
        if (errors) throw std::runtime_error("Vulkan validation errors: " + std::to_string(errors));
        std::printf("SAMPLES=%llu repeat=%llu elapsed_us=%lld\n", static_cast<unsigned long long>(samples),
            static_cast<unsigned long long>(repeat), static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count()));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
