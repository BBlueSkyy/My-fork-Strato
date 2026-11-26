// SPDX-License-Identifier: MPL-2.0
// Headless real Vulkan integration. Run with run_vulkan.py.
#include "production_barriers.inc"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        VkResult r = (x);                                                                                              \
        if (r)                                                                                                         \
            throw std::runtime_error(std::string(#x) + ":" + std::to_string(r));                                       \
    } while (0)
static int errors = 0, hazards = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        errors++;
        fprintf(stderr, "VALIDATION: %s\n", d->pMessage);
        if (strstr(d->pMessage, "SYNC-HAZARD"))
            hazards++;
    }
    return VK_FALSE;
}
struct Buffer {
    VkBuffer b;
    VkDeviceMemory m;
    void *p;
    VkDeviceSize n;
};
struct Pipeline {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorSet set;
};
struct Harness {
    VkInstance instance{};
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkDescriptorPool descriptors{};
    VkDebugUtilsMessengerEXT messenger{};
    bool conditional{};
    Harness(bool enableConditional) {
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
        const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        VkValidationFeatureEnableEXT en = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT val{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        val.enabledValidationFeatureCount = 1;
        val.pEnabledValidationFeatures = &en;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "strato-query-validation";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pNext = &val;
        ci.pApplicationInfo = &app;
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = extensions;
        VkResult instanceResult = vkCreateInstance(&ci, nullptr, &instance);
        if (instanceResult == VK_ERROR_LAYER_NOT_PRESENT || instanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
            throw std::runtime_error("unavailable Vulkan runtime: layer or ICD missing");
        CHECK(instanceResult);
        VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug;
        auto mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        CHECK(mk(instance, &di, nullptr, &messenger));
        uint32_t n = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        std::vector<VkPhysicalDevice> ps(n);
        CHECK(vkEnumeratePhysicalDevices(instance, &n, ps.data()));
        if (!n)
            throw std::runtime_error("unavailable Vulkan runtime: no devices");
        physical = ps[0];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        printf("device=%s\n", props.deviceName);
        uint32_t fn;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &fn, nullptr);
        std::vector<VkQueueFamilyProperties> fs(fn);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &fn, fs.data());
        for (family = 0; family < fn; family++)
            if ((fs[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                break;
        CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr));
        std::vector<VkExtensionProperties> es(n);
        CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, es.data()));
        bool supported = false;
        for (auto &e : es)
            if (!strcmp(e.extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME))
                supported = true;
        VkPhysicalDeviceConditionalRenderingFeaturesEXT cf{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (supported)
            feat.pNext = &cf;
        vkGetPhysicalDeviceFeatures2(physical, &feat);
        conditional = enableConditional && supported && cf.conditionalRendering;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qi;
        const char *ce = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
        if (conditional) {
            cf.inheritedConditionalRendering = VK_FALSE;
            dc.pNext = &cf;
            dc.enabledExtensionCount = 1;
            dc.ppEnabledExtensionNames = &ce;
        }
        CHECK(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        printf("conditional requested=%d supported=%d enabled=%d\n", enableConditional, supported, conditional);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 8;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &sz;
        CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
    }
    Buffer buffer(VkDeviceSize n) {
        Buffer b{};
        b.n = n;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = n;
        bi.usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (conditional)
            bi.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(device, &bi, nullptr, &b.b));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.b, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t i = 0;
        for (; i < props.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                ((props.memoryTypes[i].propertyFlags &
                  (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
                break;
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = i;
        CHECK(vkAllocateMemory(device, &mi, nullptr, &b.m));
        CHECK(vkBindBufferMemory(device, b.b, b.m, 0));
        CHECK(vkMapMemory(device, b.m, 0, n, 0, &b.p));
        memset(b.p, 0, n);
        return b;
    }
    Pipeline pipeline(const char *file, std::vector<Buffer> bs, uint32_t pushBytes) {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f)
            throw std::runtime_error(file);
        size_t size = f.tellg();
        f.seekg(0);
        std::vector<uint32_t> words(size / 4);
        f.read((char *)words.data(), size);
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = size;
        si.pCode = words.data();
        VkShaderModule shader;
        CHECK(vkCreateShaderModule(device, &si, nullptr, &shader));
        Pipeline p{};
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t i = 0; i < bs.size(); i++)
            bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = bindings.size();
        li.pBindings = bindings.data();
        CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &p.ds));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &p.ds;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        CHECK(vkCreatePipelineLayout(device, &pli, nullptr, &p.layout));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = shader;
        pci.stage.pName = "main";
        pci.layout = p.layout;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &p.pipeline));
        vkDestroyShaderModule(device, shader, nullptr);
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptors;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &p.ds;
        CHECK(vkAllocateDescriptorSets(device, &da, &p.set));
        for (uint32_t i = 0; i < bs.size(); i++) {
            VkDescriptorBufferInfo bi{bs[i].b, 0, bs[i].n};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = p.set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
        return p;
    }
    void begin() {
        CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
    }
    void end() {
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        CHECK(vkCreateFence(device, &fi, nullptr, &fence));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(device, fence, nullptr);
    }
    void barrier(VkPipelineStageFlags s, VkPipelineStageFlags d, VkAccessFlags sa, VkAccessFlags da) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = sa;
        m.dstAccessMask = da;
        vkCmdPipelineBarrier(cmd, s, d, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    void full() {
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT);
    }
    void dispatch(Pipeline &p, const void *push, uint32_t size) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
        vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    // Exact stage/access masks from QueryResolveHelperShader::Resolve and PrepareForTransfer.
    void resolve(Pipeline &p, uint32_t first, uint32_t count, bool reset) {
        barrier(RESOLVE_SRC_STAGE, RESOLVE_DST_STAGE, RESOLVE_SRC_ACCESS, RESOLVE_DST_ACCESS);
        uint32_t push[] = {first, count, reset ? 1u : 0u};
        dispatch(p, push, 12);
    }
    void prepare() { barrier(PREPARE_SRC_STAGE, PREPARE_DST_STAGE, PREPARE_SRC_ACCESS, PREPARE_DST_ACCESS); }
    void copy(Buffer a, Buffer b, VkDeviceSize n = 8, VkDeviceSize ao = 0, VkDeviceSize bo = 0) {
        VkBufferCopy cp{ao, bo, n};
        vkCmdCopyBuffer(cmd, a.b, b.b, 1, &cp);
    }
    // Buffer range and stage/access masks are extracted from ScheduleResolve by the runner.
    void report(Buffer source, Buffer destination, VkDeviceSize size, VkDeviceSize offset) {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = REPORT_BEFORE_SRC_ACCESS;
        barrier.dstAccessMask = REPORT_BEFORE_DST_ACCESS;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = destination.b;
        barrier.offset = offset;
        barrier.size = size;
        vkCmdPipelineBarrier(cmd, REPORT_BEFORE_SRC_STAGE, REPORT_BEFORE_DST_STAGE, 0, 0, nullptr, 1, &barrier, 0,
                             nullptr);
        copy(source, destination, size, 0, offset);
        barrier.srcAccessMask = REPORT_AFTER_SRC_ACCESS;
        barrier.dstAccessMask = REPORT_AFTER_DST_ACCESS;
        vkCmdPipelineBarrier(cmd, REPORT_AFTER_SRC_STAGE, REPORT_AFTER_DST_STAGE, 0, 0, nullptr, 1, &barrier, 0,
                             nullptr);
    }
    void destroy(Buffer b) {
        vkUnmapMemory(device, b.m);
        vkDestroyBuffer(device, b.b, nullptr);
        vkFreeMemory(device, b.m, nullptr);
    }
    void destroy(Pipeline p) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
        vkDestroyPipelineLayout(device, p.layout, nullptr);
        vkDestroyDescriptorSetLayout(device, p.ds, nullptr);
    }
    ~Harness() {
        vkDeviceWaitIdle(device);
        vkDestroyDescriptorPool(device, descriptors, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        auto fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        fn(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
int main(int argc, char **argv) {
    try {
        bool ext = argc > 1 && !strcmp(argv[1], "conditional");
        Harness h(ext);
        auto results = h.buffer(QUERY_POOL_SIZE * 8);
        auto acc = h.buffer(8);
        auto out = h.buffer(128 * 8);
        auto scratch = h.buffer(20);
        auto p = h.pipeline("query_resolve.spv", {results, acc}, 12);
        auto cond = h.pipeline("conditional_compare.spv", {scratch}, 8);
        auto val = (uint64_t *)results.p;
        val[0] = 0xffffffffull;
        val[1] = 2;
        val[2] = 0x100000003ull;
        val[3] = 0xfffffffffffffff9ull;
        h.begin();
        h.full();
        h.resolve(p, 0, 2, true);
        h.resolve(p, 2, 1, false);
        h.prepare();
        h.copy(acc, out);
        h.full();
        h.resolve(p, 3, 1, false);
        h.prepare();
        h.copy(acc, out, 8, 0, 8);
        h.full();
        h.end();
        if (((uint64_t *)out.p)[0] != 0x200000004ull || ((uint64_t *)out.p)[1] != 0x1fffffffdull)
            throw std::runtime_error("carry/segmented sum failed");
        printf("PASS carry, segmented accumulation, 64-bit wrapping\n");
        int before = hazards;
        h.begin();
        h.full();
        h.resolve(p, 0, 1, true);
        h.prepare();
        h.report(acc, out, 8, 0);
        h.resolve(p, 1, 1, false);
        h.prepare();
        h.report(acc, out, 4, 0);
        h.full();
        h.end();
        printf("CASE overlapping 64/32-bit reports hazards=%d\n", hazards - before);
        before = hazards;
        h.begin();
        h.full();
        h.resolve(p, 0, 1, true);
        h.prepare();
        h.copy(acc, out);
        h.barrier(RESET_SRC_STAGE, RESET_DST_STAGE, RESET_SRC_ACCESS, RESET_DST_ACCESS);
        vkCmdFillBuffer(h.cmd, acc.b, 0, 8, 0);
        h.prepare();
        h.copy(acc, out, 8, 0, 8);
        h.full();
        h.end();
        printf("CASE report-reset-empty-report hazards=%d output=%llu\n", hazards - before,
               (unsigned long long)((uint64_t *)out.p)[1]);
        before = hazards;
        h.begin();
        h.full();
        h.resolve(p, 0, 1, true);
        h.barrier(RESET_SRC_STAGE, RESET_DST_STAGE, RESET_SRC_ACCESS, RESET_DST_ACCESS);
        vkCmdFillBuffer(h.cmd, acc.b, 0, 8, 0);
        h.prepare();
        h.copy(acc, out);
        h.full();
        h.end();
        printf("CASE compact-reset-empty-report hazards=%d output=%llu\n", hazards - before,
               (unsigned long long)((uint64_t *)out.p)[0]);
        before = hazards;
        h.begin();
        h.full();
        vkCmdFillBuffer(h.cmd, acc.b, 0, 8, 0);
        h.prepare();
        h.copy(acc, out);
        h.barrier(RESET_SRC_STAGE, RESET_DST_STAGE, RESET_SRC_ACCESS, RESET_DST_ACCESS);
        vkCmdFillBuffer(h.cmd, acc.b, 0, 8, 0);
        h.prepare();
        h.copy(acc, out, 8, 0, 8);
        h.full();
        h.end();
        printf("CASE consecutive-empty-reports hazards=%d\n", hazards - before);
        std::vector<std::array<uint64_t, 5>> predicates = {{0, 0, 0, 0, 1},
                                                           {0x100000013ull, 0x200000013ull, 0, 0, 0},
                                                           {0x100000013ull, 0x100000013ull, 1, 0, 0},
                                                           {0x100000013ull, 0x200000013ull, 1, 0, 1},
                                                           {0x100000000ull, 17, 0, 1, 0},
                                                           {19, 0, 0, 1, 1},
                                                           {0, 0, 0, 1, 0}};
        for (auto tc : predicates) {
            uint32_t *s = (uint32_t *)scratch.p;
            ((uint64_t *)s)[0] = tc[0];
            ((uint64_t *)s)[1] = tc[1];
            s[4] = 0xabcd;
            uint32_t push[] = {uint32_t(tc[2]), uint32_t(tc[3])};
            h.begin();
            h.full();
            h.dispatch(cond, push, 8);
            h.full();
            h.end();
            if (s[4] != tc[4])
                throw std::runtime_error("conditional predicate failed");
        }
        printf("PASS 64-bit conditional EQ/NE and existing low-word NonZero semantics\n");

        // Empty render pass queries exercise real query reset/begin/end/copy and fence-qualified reuse.
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
        qpci.queryCount = QUERY_POOL_SIZE;
        VkQueryPool qp;
        CHECK(vkCreateQueryPool(h.device, &qpci, nullptr, &qp));
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        VkRenderPass rp;
        CHECK(vkCreateRenderPass(h.device, &rpci, nullptr, &rp));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.width = 1;
        fci.height = 1;
        fci.layers = 1;
        VkFramebuffer fb;
        CHECK(vkCreateFramebuffer(h.device, &fci, nullptr, &fb));
        for (int reuse = 0; reuse < 2; reuse++) {
            h.begin();
            h.full();
            vkCmdResetQueryPool(h.cmd, qp, 0, QUERY_POOL_SIZE);
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = rp;
            rbi.framebuffer = fb;
            rbi.renderArea.extent = {1, 1};
            vkCmdBeginRenderPass(h.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            for (uint32_t q = 0; q < QUERY_POOL_SIZE; q++) {
                vkCmdBeginQuery(h.cmd, qp, q, 0);
                vkCmdEndQuery(h.cmd, qp, q);
            }
            vkCmdEndRenderPass(h.cmd);
            vkCmdCopyQueryPoolResults(h.cmd, qp, 0, QUERY_POOL_SIZE, results.b, 0, 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            h.resolve(p, 0, QUERY_POOL_SIZE, true);
            h.prepare();
            h.copy(acc, out);
            h.full();
            h.end();
            if (*(uint64_t *)out.p != 0)
                throw std::runtime_error("empty query sum failed");
        }
        printf("PASS %u occlusion query batch reset/begin/end/copy/resolve twice with fence reuse\n", QUERY_POOL_SIZE);
        auto shader = [&](const char *file) {
            std::ifstream f(file, std::ios::binary | std::ios::ate);
            size_t n = f.tellg();
            f.seekg(0);
            std::vector<uint32_t> v(n / 4);
            f.read((char *)v.data(), n);
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = n;
            ci.pCode = v.data();
            VkShaderModule s;
            CHECK(vkCreateShaderModule(h.device, &ci, nullptr, &s));
            return s;
        };
        VkShaderModule vs = shader("triangle.vert.spv"), fs = shader("triangle.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &s : stages)
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, 1, 1, 0, 1};
        VkRect2D scissor{{0, 0}, {1, 1}};
        VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        view.viewportCount = 1;
        view.pViewports = &viewport;
        view.scissorCount = 1;
        view.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineLayoutCreateInfo glci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout gl;
        CHECK(vkCreatePipelineLayout(h.device, &glci, nullptr, &gl));
        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vertex;
        gpci.pInputAssemblyState = &assembly;
        gpci.pViewportState = &view;
        gpci.pRasterizationState = &raster;
        gpci.pMultisampleState = &ms;
        gpci.pColorBlendState = &blend;
        gpci.layout = gl;
        gpci.renderPass = rp;
        VkPipeline gp;
        CHECK(vkCreateGraphicsPipelines(h.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &gp));
        auto beginConditional =
            (PFN_vkCmdBeginConditionalRenderingEXT)vkGetDeviceProcAddr(h.device, "vkCmdBeginConditionalRenderingEXT");
        auto endConditional =
            (PFN_vkCmdEndConditionalRenderingEXT)vkGetDeviceProcAddr(h.device, "vkCmdEndConditionalRenderingEXT");
        for (auto tc : predicates) {
            uint32_t *s = (uint32_t *)scratch.p;
            ((uint64_t *)s)[0] = tc[0];
            ((uint64_t *)s)[1] = tc[1];
            s[4] = 0xcd;
            uint32_t push[] = {uint32_t(tc[2]), uint32_t(tc[3])};
            h.begin();
            h.full();
            h.dispatch(cond, push, 8);
            bool cpuDraw = true;
            if (h.conditional)
                h.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT);
            else {
                h.full();
                h.end();
                cpuDraw = s[4] != 0;
                h.begin();
                h.full();
            }
            vkCmdResetQueryPool(h.cmd, qp, 0, 1);
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = rp;
            rbi.framebuffer = fb;
            rbi.renderArea.extent = {1, 1};
            vkCmdBeginRenderPass(h.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(h.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
            vkCmdBeginQuery(h.cmd, qp, 0, 0);
            if (h.conditional) {
                VkConditionalRenderingBeginInfoEXT cbi{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
                cbi.buffer = scratch.b;
                cbi.offset = 16;
                beginConditional(h.cmd, &cbi);
            }
            if (h.conditional || cpuDraw)
                vkCmdDraw(h.cmd, 3, 1, 0, 0);
            if (h.conditional)
                endConditional(h.cmd);
            vkCmdEndQuery(h.cmd, qp, 0);
            vkCmdEndRenderPass(h.cmd);
            vkCmdCopyQueryPoolResults(h.cmd, qp, 0, 1, results.b, 0, 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            h.resolve(p, 0, 1, true);
            h.prepare();
            h.copy(acc, out);
            h.report(acc, scratch, 4, 16);
            h.copy(scratch, out, 4, 16, 32);
            h.full();
            h.end();
            bool expected = tc[4] != 0;
            uint64_t actual = *(uint64_t *)out.p;
            printf("triangle notEqual=%llu nonZero=%llu expectedDraw=%d samples=%llu\n", (unsigned long long)tc[2],
                   (unsigned long long)tc[3], expected, (unsigned long long)actual);
            if (((uint32_t *)out.p)[8] != uint32_t(actual))
                throw std::runtime_error("report to DMA consumer mismatch");
            if ((actual > 0) != expected)
                throw std::runtime_error("conditional triangle occlusion mismatch");
        }
        printf("PASS conditional predicate read to report overwrite to DMA consumer\n");
        printf("PASS real triangle occlusion with conditional extension %s\n",
               h.conditional ? "enabled" : "disabled with synchronized CPU predicate read");
        vkDestroyPipeline(h.device, gp, nullptr);
        vkDestroyPipelineLayout(h.device, gl, nullptr);
        vkDestroyShaderModule(h.device, vs, nullptr);
        vkDestroyShaderModule(h.device, fs, nullptr);

        vkDestroyFramebuffer(h.device, fb, nullptr);
        vkDestroyRenderPass(h.device, rp, nullptr);
        vkDestroyQueryPool(h.device, qp, nullptr);
        h.destroy(p);
        h.destroy(cond);
        h.destroy(results);
        h.destroy(acc);
        h.destroy(out);
        h.destroy(scratch);
        printf("RESULT errors=%d hazards=%d\n", errors, hazards);
        return errors ? 2 : 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
