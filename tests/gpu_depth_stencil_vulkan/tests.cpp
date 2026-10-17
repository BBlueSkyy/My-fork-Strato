// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <boost/container/small_vector.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <gpu/texture/depth_stencil.h>

using u32 = uint32_t;
using u8 = uint8_t;
static unsigned validationErrors{};
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

namespace skyline::gpu {
namespace texture {
struct Level { vk::Extent3D dimensions; size_t targetLinearSize; };
}
namespace memory {
struct StagingBuffer {
    vk::raii::DeviceMemory allocation{nullptr};
    vk::raii::Buffer buffer{nullptr};
    vk::Buffer vkBuffer{};
    size_t allocationSize{};
    u8 *pointer{};
    size_t size() const { return allocationSize; }
};
}
struct NativeFormat { vk::ImageAspectFlags vkAspect; size_t bpb; };
struct Texture {
    NativeFormat *format;
    std::vector<texture::Level> mipLayouts;
    u32 layerCount{3}, levelCount{4};
    size_t surfaceSize{};
    vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    vk::Image image{};
    vk::Image GetBacking() const { return image; }
    boost::container::small_vector<vk::BufferImageCopy, 10> GetBufferImageCopies();
    void CopyFromStagingBuffer(const vk::raii::CommandBuffer &, const std::shared_ptr<memory::StagingBuffer> &);
    void CopyIntoStagingBuffer(const vk::raii::CommandBuffer &, const std::shared_ptr<memory::StagingBuffer> &);
};
#include "production.inc"

struct Harness {
    const vk::raii::Device &device;
    const vk::raii::PhysicalDevice &physical;
    vk::raii::Queue queue;
    vk::raii::CommandPool pool;
    Harness(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physical, u32 family)
        : device{device}, physical{physical}, queue{device, family, 0},
          pool{device, vk::CommandPoolCreateInfo{.queueFamilyIndex = family}} {}
    u32 MemoryType(u32 bits, vk::MemoryPropertyFlags flags = {}) {
        auto properties{physical.getMemoryProperties()};
        for (u32 i{}; i < properties.memoryTypeCount; ++i)
            if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No suitable memory type");
    }
    std::shared_ptr<memory::StagingBuffer> Buffer(size_t size) {
        auto result{std::make_shared<memory::StagingBuffer>()};
        result->allocationSize = size;
        result->buffer = vk::raii::Buffer{device, vk::BufferCreateInfo{.size = size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer}};
        auto requirements{result->buffer.getMemoryRequirements()};
        result->allocation = vk::raii::DeviceMemory{device, vk::MemoryAllocateInfo{
            .allocationSize = requirements.size, .memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)}};
        result->buffer.bindMemory(*result->allocation, 0);
        result->vkBuffer = *result->buffer;
        result->pointer = static_cast<u8 *>(result->allocation.mapMemory(0, size));
        return result;
    }
    template<typename F> void Submit(F &&record) {
        auto commands{device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
            .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1})};
        auto &cmd{commands.front()};
        cmd.begin(vk::CommandBufferBeginInfo{});
        record(cmd);
        cmd.end();
        vk::raii::Fence fence{device, vk::FenceCreateInfo{}};
        queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &*cmd}, *fence);
        if (device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
            throw std::runtime_error("Readback fence failed");
    }
};

static void Store(std::vector<u8> &bytes, size_t offset, u32 value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void CheckSingleAspect(Harness &h, vk::Format vkFormat, vk::ImageAspectFlagBits aspect, size_t bpb) {
    NativeFormat format{aspect, bpb};
    Texture texture{};
    texture.format = &format;
    texture.levelCount = 3;
    for (u32 mip{}; mip < texture.levelCount; ++mip) {
        vk::Extent3D extent{std::max(3U >> mip, 1U), std::max(5U >> mip, 1U), 1};
        texture.mipLayouts.push_back({extent, extent.width * extent.height * bpb});
        texture.surfaceSize += texture.mipLayouts.back().targetLinearSize * texture.layerCount;
    }
    vk::raii::Image image{h.device, vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, .format = vkFormat,
        .extent = {3, 5, 1}, .mipLevels = texture.levelCount, .arrayLayers = texture.layerCount,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst}};
    auto requirements{image.getMemoryRequirements()};
    vk::raii::DeviceMemory allocation{h.device, vk::MemoryAllocateInfo{.allocationSize = requirements.size,
        .memoryTypeIndex = h.MemoryType(requirements.memoryTypeBits)}};
    image.bindMemory(*allocation, 0);
    texture.image = *image;
    auto transferSize{texture::GetSingleAspectTransferSize(texture.mipLayouts, texture.layerCount)};
    auto upload{h.Buffer(transferSize)}, readback{h.Buffer(transferSize)};
    std::vector<u8> expected(texture.surfaceSize);
    for (size_t i{}; i < expected.size(); ++i) expected[i] = static_cast<u8>(i * 17 + 3);
    std::memcpy(upload->pointer, expected.data(), expected.size());
    texture::UnpackSingleAspect(std::span<u8>{upload->pointer, transferSize}, texture.mipLayouts, texture.layerCount);
    auto regions{texture.GetBufferImageCopies()};
    assert(regions.size() == texture.levelCount);
    for (const auto &region : regions) assert(region.bufferOffset % 4 == 0);
    h.Submit([&](const vk::raii::CommandBuffer &cmd) {
        texture.CopyFromStagingBuffer(cmd, upload);
        texture.CopyIntoStagingBuffer(cmd, readback);
    });
    texture::PackSingleAspect(std::span<u8>{readback->pointer, transferSize}, texture.mipLayouts, texture.layerCount);
    assert(!std::memcmp(readback->pointer, expected.data(), expected.size()));
    upload->allocation.unmapMemory();
    readback->allocation.unmapMemory();
    std::printf("format=%s: odd dimensions, aligned 3-mip 3-layer single-aspect roundtrip passed\n", vk::to_string(vkFormat).c_str());
}

void CheckFormat(Harness &h, vk::Format vkFormat, texture::DepthStencilPacking packing, size_t bpb) {
    NativeFormat format{vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil, bpb};
    Texture texture{};
    texture.format = &format;
    for (u32 level{}; level < texture.levelCount; ++level) {
        vk::Extent3D extent{std::max(8U >> level, 1U), std::max(4U >> level, 1U), 1};
        texture.mipLayouts.push_back({extent, extent.width * extent.height * bpb});
        texture.surfaceSize += texture.mipLayouts.back().targetLinearSize * texture.layerCount;
    }
    vk::raii::Image image{h.device, vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, .format = vkFormat,
        .extent = {8, 4, 1}, .mipLevels = texture.levelCount, .arrayLayers = texture.layerCount,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled}};
    auto requirements{image.getMemoryRequirements()};
    vk::raii::DeviceMemory allocation{h.device, vk::MemoryAllocateInfo{.allocationSize = requirements.size,
        .memoryTypeIndex = h.MemoryType(requirements.memoryTypeBits)}};
    image.bindMemory(*allocation, 0);
    texture.image = *image;
    auto transferSize{texture::GetDepthStencilTransferSize(texture.surfaceSize, texture.mipLayouts,
        texture.layerCount, bpb)};
    auto upload{h.Buffer(transferSize)}, readback{h.Buffer(transferSize)};
    std::vector<u8> expected(texture.surfaceSize);
    size_t offset{};
    for (u32 mip{}; mip < texture.levelCount; ++mip) {
        size_t pixels{texture.mipLayouts[mip].targetLinearSize / bpb};
        for (u32 layer{}; layer < texture.layerCount; ++layer)
            for (size_t pixel{}; pixel < pixels; ++pixel) {
                u32 stencil{static_cast<u32>((mip * 59 + layer * 37 + pixel * 3 + 1) & 255)};
                u32 depth{static_cast<u32>((mip * 1023 + layer * 997 + pixel * 311 + 1) & 0xffffff)};
                if (packing == texture::DepthStencilPacking::Depth32FloatStencil8) {
                    float value{(mip + layer + pixel % 7 + 1) / 16.0f};
                    std::memcpy(expected.data() + offset, &value, 4);
                    Store(expected, offset + 4, stencil);
                } else if (packing == texture::DepthStencilPacking::Depth24Stencil8) {
                    Store(expected, offset, depth | (stencil << 24));
                } else {
                    Store(expected, offset, stencil | (depth << 8));
                }
                offset += bpb;
            }
    }
    auto regions{texture.GetBufferImageCopies()};
    assert(regions.size() == 2 * texture.levelCount);
    for (const auto &region : regions) {
        assert(region.bufferOffset % 4 == 0);
        assert(region.imageSubresource.layerCount == texture.layerCount);
        assert(region.imageSubresource.mipLevel < texture.levelCount);
        auto aspect{region.imageSubresource.aspectMask};
        assert(aspect == vk::ImageAspectFlagBits::eDepth || aspect == vk::ImageAspectFlagBits::eStencil);
        size_t texels{region.imageExtent.width * region.imageExtent.height * texture.layerCount};
        assert(region.bufferOffset + texels * (aspect == vk::ImageAspectFlagBits::eDepth ? 4 : 1) <= transferSize);
    }
    auto prepareUpload = [&] {
        std::memset(upload->pointer, 0, transferSize);
        std::memcpy(upload->pointer, expected.data(), expected.size());
        texture::UnpackDepthStencil(std::span<u8>{upload->pointer, transferSize}, packing,
            texture.mipLayouts, texture.layerCount, texture.surfaceSize);
    };
    auto checkReadback = [&] {
        texture::PackDepthStencil(std::span<u8>{readback->pointer, transferSize}, packing,
            texture.mipLayouts, texture.layerCount, texture.surfaceSize);
        if (std::memcmp(readback->pointer, expected.data(), expected.size()))
            throw std::runtime_error("Depth/stencil readback differs from packed reference");
    };
    prepareUpload();
    h.Submit([&](const vk::raii::CommandBuffer &cmd) {
        texture.CopyFromStagingBuffer(cmd, upload);
        texture.CopyIntoStagingBuffer(cmd, readback);
    });
    checkReadback();
    // Re-upload while GENERAL: regression coverage for the prior WAW hazard.
    h.Submit([&](const vk::raii::CommandBuffer &cmd) {
        texture.CopyFromStagingBuffer(cmd, upload);
        texture.CopyFromStagingBuffer(cmd, upload);
        texture.CopyIntoStagingBuffer(cmd, readback);
    });
    checkReadback();
    for (bool depthOnly : {true, false}) {
        // Only mip 2, array layer 1 changes. All other subresources and the
        // opposite aspect must retain their distinct original texel values.
        size_t selected{};
        for (u32 mip{}; mip < 2; ++mip)
            selected += texture.mipLayouts[mip].targetLinearSize * texture.layerCount;
        selected += texture.mipLayouts[2].targetLinearSize;
        for (size_t pixel{}; pixel < texture.mipLayouts[2].targetLinearSize / bpb; ++pixel) {
            size_t index{selected + pixel * bpb};
            u32 old{};
            std::memcpy(&old, expected.data() + index, 4);
            if (depthOnly) {
                if (bpb == 8) { float value{0.0f}; std::memcpy(expected.data() + index, &value, 4); }
                else Store(expected, index, packing == texture::DepthStencilPacking::Depth24Stencil8 ? old & 0xff000000 : old & 0xff);
            } else {
                if (bpb == 8) Store(expected, index + 4, 173);
                else Store(expected, index, packing == texture::DepthStencilPacking::Depth24Stencil8 ?
                    (old & 0xffffff) | (173U << 24) : (old & 0xffffff00) | 173U);
            }
        }
        h.Submit([&](const vk::raii::CommandBuffer &cmd) {
            cmd.clearDepthStencilImage(texture.image, vk::ImageLayout::eGeneral, vk::ClearDepthStencilValue{0.0f, 173},
                vk::ImageSubresourceRange{.aspectMask = depthOnly ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eStencil,
                    .baseMipLevel = 2, .levelCount = 1, .baseArrayLayer = 1, .layerCount = 1});
            texture.CopyIntoStagingBuffer(cmd, readback);
        });
        checkReadback();
    }
    // Exercise an actual attachment writer and preserve stencil with LOAD.
    vk::raii::ImageView view{h.device, vk::ImageViewCreateInfo{.image = *image,
        .viewType = vk::ImageViewType::e2D, .format = vkFormat,
        .subresourceRange = {.aspectMask = format.vkAspect, .baseMipLevel = 1, .levelCount = 1,
            .baseArrayLayer = 2, .layerCount = 1}}};
    const vk::AttachmentDescription attachment{.format = vkFormat, .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eLoad, .stencilStoreOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eGeneral, .finalLayout = vk::ImageLayout::eGeneral};
    const vk::AttachmentReference reference{.attachment = 0, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal};
    const vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .pDepthStencilAttachment = &reference};
    const auto attachmentStages{vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests};
    const auto attachmentAccess{vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite};
    const std::array dependencies{
        vk::SubpassDependency{.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eAllCommands, .dstStageMask = attachmentStages,
            .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = attachmentAccess},
        vk::SubpassDependency{.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = attachmentStages, .dstStageMask = vk::PipelineStageFlagBits::eAllCommands,
            .srcAccessMask = attachmentAccess, .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite}};
    vk::raii::RenderPass renderPass{h.device, vk::RenderPassCreateInfo{.attachmentCount = 1,
        .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = static_cast<u32>(dependencies.size()), .pDependencies = dependencies.data()}};
    const vk::ImageView rawView{*view};
    vk::raii::Framebuffer framebuffer{h.device, vk::FramebufferCreateInfo{.renderPass = *renderPass,
        .attachmentCount = 1, .pAttachments = &rawView, .width = 4, .height = 2, .layers = 1}};
    vk::raii::ImageView sampledView{h.device, vk::ImageViewCreateInfo{.image = *image,
        .viewType = vk::ImageViewType::e2D, .format = vkFormat,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eDepth, .baseMipLevel = 1, .levelCount = 1,
            .baseArrayLayer = 2, .layerCount = 1}}};
    auto samples{h.Buffer(8 * sizeof(float))};
    std::memset(samples->pointer, 0xff, samples->size());
    vk::raii::Sampler sampler{h.device, vk::SamplerCreateInfo{.magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest, .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge, .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge}};
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute}};
    vk::raii::DescriptorSetLayout descriptorLayout{h.device, vk::DescriptorSetLayoutCreateInfo{
        .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()}};
    const vk::DescriptorSetLayout rawLayout{*descriptorLayout};
    vk::raii::PipelineLayout pipelineLayout{h.device, vk::PipelineLayoutCreateInfo{
        .setLayoutCount = 1, .pSetLayouts = &rawLayout}};
    const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1}};
    vk::raii::DescriptorPool descriptors{h.device, vk::DescriptorPoolCreateInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, .maxSets = 1,
        .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()}};
    auto sets{h.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
        .descriptorPool = *descriptors, .descriptorSetCount = 1, .pSetLayouts = &rawLayout})};
    const vk::DescriptorImageInfo imageInfo{.sampler = *sampler, .imageView = *sampledView, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo bufferInfo{.buffer = samples->vkBuffer, .offset = 0, .range = samples->size()};
    const std::array writes{
        vk::WriteDescriptorSet{.dstSet = *sets.front(), .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo},
        vk::WriteDescriptorSet{.dstSet = *sets.front(), .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &bufferInfo}};
    h.device.updateDescriptorSets(writes, {});
    std::ifstream shaderFile{"sample.spv", std::ios::binary | std::ios::ate};
    std::vector<u32> code(static_cast<size_t>(shaderFile.tellg()) / sizeof(u32));
    shaderFile.seekg(0);
    shaderFile.read(reinterpret_cast<char *>(code.data()), code.size() * sizeof(u32));
    vk::raii::ShaderModule module{h.device, vk::ShaderModuleCreateInfo{.codeSize = code.size() * sizeof(u32), .pCode = code.data()}};
    auto pipeline{h.device.createComputePipeline(nullptr, vk::ComputePipelineCreateInfo{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"}, .layout = *pipelineLayout})};
    size_t attachmentOffset{texture.mipLayouts[0].targetLinearSize * texture.layerCount +
        texture.mipLayouts[1].targetLinearSize * 2};
    for (size_t pixel{}; pixel < texture.mipLayouts[1].targetLinearSize / bpb; ++pixel) {
        size_t index{attachmentOffset + pixel * bpb};
        u32 old{};
        std::memcpy(&old, expected.data() + index, 4);
        Store(expected, index, bpb == 8 ? 0 : packing == texture::DepthStencilPacking::Depth24Stencil8 ?
            old & 0xff000000 : old & 0xff);
    }
    h.Submit([&](const vk::raii::CommandBuffer &cmd) {
        vk::ClearValue clear{};
        clear.depthStencil = vk::ClearDepthStencilValue{0.0f, 0};
        cmd.beginRenderPass(vk::RenderPassBeginInfo{.renderPass = *renderPass, .framebuffer = *framebuffer,
            .renderArea = {{0, 0}, {4, 2}}, .clearValueCount = 1, .pClearValues = &clear}, vk::SubpassContents::eInline);
        cmd.endRenderPass();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, *sets.front(), {});
        cmd.dispatch(8, 1, 1);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {}, {},
            vk::BufferMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite, .dstAccessMask = vk::AccessFlagBits::eHostRead,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = samples->vkBuffer, .offset = 0, .size = samples->size()}, {});
        texture.CopyIntoStagingBuffer(cmd, readback);
    });
    checkReadback();
    for (size_t i{}; i < 8; ++i) {
        float sampled{};
        std::memcpy(&sampled, samples->pointer + i * sizeof(float), sizeof(float));
        assert(sampled == 0.0f);
    }
    samples->allocation.unmapMemory();
    upload->allocation.unmapMemory();
    readback->allocation.unmapMemory();
    std::printf("format=%s packing=%u mips=%u layers=%u: upload/readback, repeat upload, independent aspect and attachment clears passed\n",
        vk::to_string(vkFormat).c_str(), static_cast<unsigned>(packing), texture.levelCount, texture.layerCount);
}
}

int main() {
    try {
        vk::raii::Context context;
        const char *layer{"VK_LAYER_KHRONOS_validation"};
        const char *extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const vk::ValidationFeatureEnableEXT synchronization{vk::ValidationFeatureEnableEXT::eSynchronizationValidation};
        const vk::ValidationFeaturesEXT validation{.enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &synchronization};
        const vk::ApplicationInfo app{.apiVersion = VK_API_VERSION_1_1};
        vk::raii::Instance instance{context, vk::InstanceCreateInfo{.pNext = &validation, .pApplicationInfo = &app,
            .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions}};
        vk::raii::DebugUtilsMessengerEXT debug{instance, vk::DebugUtilsMessengerCreateInfoEXT{
            .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation, .pfnUserCallback = ValidationMessage}};
        {
            vk::raii::PhysicalDevice physical{std::move(instance.enumeratePhysicalDevices().front())};
            std::printf("Vulkan 1.1 device=%s\n", physical.getProperties().deviceName.data());
            auto families{physical.getQueueFamilyProperties()};
            u32 family{};
            while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) ++family;
            const float priority{1};
            const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
            vk::raii::Device device{physical, vk::DeviceCreateInfo{.queueCreateInfoCount = 1, .pQueueCreateInfos = &queue}};
            skyline::gpu::Harness harness{device, physical, family};
            using skyline::gpu::texture::DepthStencilPacking;
            skyline::gpu::CheckFormat(harness, vk::Format::eD24UnormS8Uint, DepthStencilPacking::Depth24Stencil8, 4);
            skyline::gpu::CheckFormat(harness, vk::Format::eD24UnormS8Uint, DepthStencilPacking::Stencil8Depth24, 4);
            skyline::gpu::CheckFormat(harness, vk::Format::eD32SfloatS8Uint, DepthStencilPacking::Depth32FloatStencil8, 8);
            skyline::gpu::CheckSingleAspect(harness, vk::Format::eD16Unorm, vk::ImageAspectFlagBits::eDepth, 2);
            skyline::gpu::CheckSingleAspect(harness, vk::Format::eS8Uint, vk::ImageAspectFlagBits::eStencil, 1);
        }
        if (validationErrors) throw std::runtime_error("Vulkan validation errors");
        std::puts("PASS: production methods, packed data, all aspects/mips/layers; zero validation/synchronization errors");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
