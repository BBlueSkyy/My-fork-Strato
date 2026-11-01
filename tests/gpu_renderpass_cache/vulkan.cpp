// SPDX-License-Identifier: MPL-2.0
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <thread>
#include <atomic>
namespace skyline::gpu {
class GPU {
  public:
    const vk::raii::Device &vkDevice;
};
}
#define main KeyFixtureMain
#include "tests.cpp"
#undef main
namespace skyline::gpu::cache {
#include "vulkan-production.inc"
}
static std::atomic<unsigned> validationErrors{};
VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::fprintf(stderr,"%s\n",data->pMessage);
    }
    return VK_FALSE;
}
int main() {
    vk::raii::Context context;
    const char *layer="VK_LAYER_KHRONOS_validation";
    const char *extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const vk::ApplicationInfo application{.apiVersion=VK_API_VERSION_1_1};
    vk::raii::Instance instance{context,vk::InstanceCreateInfo{.pApplicationInfo=&application,
        .enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension}};
    vk::raii::DebugUtilsMessengerEXT debug{instance,vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,.pfnUserCallback=Message}};
    auto physical=std::move(instance.enumeratePhysicalDevices().front());
    const auto families=physical.getQueueFamilyProperties();
    u32 family{};
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) ++family;
    const float priority=1;
    const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    vk::raii::Device device{physical,vk::DeviceCreateInfo{.queueCreateInfoCount=1,.pQueueCreateInfos=&queue}};
    skyline::gpu::GPU gpu{device};
    unsigned formats{};
    {
        Cache cache{gpu};
        for (auto depth : {vk::Format::eD16Unorm,vk::Format::eD32Sfloat,vk::Format::eD24UnormS8Uint,vk::Format::eD32SfloatS8Uint}) {
            if (!(physical.getFormatProperties(depth).optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)) continue;
            ++formats;
            Fixture f;
            f.attachments[1].format=depth;
            for(auto &attachment:f.attachments) {
                attachment.samples=vk::SampleCountFlagBits::e1;
                attachment.loadOp=attachment.stencilLoadOp=vk::AttachmentLoadOp::eClear;
                attachment.storeOp=attachment.stencilStoreOp=vk::AttachmentStoreOp::eStore;
                attachment.finalLayout=vk::ImageLayout::eGeneral;
            }
            f.subpasses[0].pDepthStencilAttachment=&f.references[1];
            const auto original=cache.GetRenderPass(f.info);
            Check(original && original==cache.GetRenderPass(f.info),"native render-pass warm lookup changed handle");
            f.references[1].layout=vk::ImageLayout::eGeneral;
            const auto general=cache.GetRenderPass(f.info);
            Check(general!=original,"native render-pass cache reused different depth layout");
            f.references[1].attachment=VK_ATTACHMENT_UNUSED;
            const auto unused=cache.GetRenderPass(f.info);
            Check(unused!=general,"native render-pass cache reused different depth attachment");
            f.references[1].attachment=1;
            const auto size=cache.renderPassCache.size();
            // The cache mutex protects both metadata and Vulkan handle lifetime.
            auto reader=[&] { for(unsigned i{};i<500;++i)
                Check(cache.GetRenderPass(f.info)==general,"concurrent native cache lookup changed handle"); };
            std::thread first{reader},second{reader}; first.join();second.join();
            Check(cache.renderPassCache.size()==size,"concurrent warm cache lookup changed cardinality");
        }
        Check(formats>0,"no native depth/stencil format tested");
        // Cached Vulkan handles are owned until cache teardown inside the device lifetime.
    }
    Check(validationErrors==0,"native render-pass Vulkan validation errors");
    std::printf("PASS real Vulkan render-pass cache: %u depth/stencil formats, distinct layouts/attachments, concurrent handles and cache lifetime; zero validation errors\n",formats);
}
