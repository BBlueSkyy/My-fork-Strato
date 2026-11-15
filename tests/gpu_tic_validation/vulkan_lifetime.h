// SPDX-License-Identifier: MPL-2.0
// Test-only Vulkan ownership facade for the real pipeline-manager/cache methods.
#pragma once
#include <vulkan/vulkan.h>
#include <fstream>
#include <string>
#undef VK_NULL_HANDLE

struct PipelineLifetimeRuntime;
struct PipelineLifetimeToken {
    PipelineLifetimeRuntime *runtime;
    VkPipeline pipeline;
    ~PipelineLifetimeToken();
};
struct PipelineLifetimeRuntime {
    static inline PipelineLifetimeRuntime *active{};
    static inline unsigned errors{};
    unsigned live{}, created{};
    VkInstance instance{}; VkDebugUtilsMessengerEXT debug{};
    VkDevice device{}; VkQueue queue{}; VkPipelineLayout layout{}; VkShaderModule module{};
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{}; VkSemaphore release{};
    static void Check(VkResult result) { if(result!=VK_SUCCESS) { std::fprintf(stderr,"lifetime Vulkan error %d\n", result); std::exit(2); } }
    static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void*) {
        if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++errors; std::fprintf(stderr,"%s\n",data->pMessage); } return VK_FALSE;
    }
    explicit PipelineLifetimeRuntime(const char *path) {
        const char *layer="VK_LAYER_KHRONOS_validation", *extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; debugInfo.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT; debugInfo.pfnUserCallback=Message;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app; ci.pNext=&debugInfo; ci.enabledLayerCount=ci.enabledExtensionCount=1; ci.ppEnabledLayerNames=&layer; ci.ppEnabledExtensionNames=&extension;
        Check(vkCreateInstance(&ci,nullptr,&instance));
        auto createDebug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        Check(createDebug(instance,&debugInfo,nullptr,&debug));
        uint32_t count{}; Check(vkEnumeratePhysicalDevices(instance,&count,nullptr)); if(!count) std::exit(2);
        std::vector<VkPhysicalDevice> physical(count); Check(vkEnumeratePhysicalDevices(instance,&count,physical.data()));
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=&timeline; vkGetPhysicalDeviceFeatures2(physical[0],&features); if(!timeline.timelineSemaphore) std::exit(2);
        vkGetPhysicalDeviceQueueFamilyProperties(physical[0],&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical[0],&count,families.data());
        uint32_t family{}; while(family<count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family; if(family==count) std::exit(2);
        float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext=&timeline; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
        Check(vkCreateDevice(physical[0],&di,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; Check(vkCreatePipelineLayout(device,&li,nullptr,&layout));
        std::ifstream file(path,std::ios::binary|std::ios::ate); if(!file) std::exit(2);
        auto size=static_cast<size_t>(file.tellg()); if(size%4) std::exit(2); std::vector<uint32_t> code(size/4); file.seekg(0); file.read(reinterpret_cast<char*>(code.data()),size);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize=size; mi.pCode=code.data(); Check(vkCreateShaderModule(device,&mi,nullptr,&module));
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=family; Check(vkCreateCommandPool(device,&pi,nullptr,&pool));
        active=this;
    }
    std::unique_ptr<PipelineLifetimeToken> Create() {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=module; ci.stage.pName="main"; ci.layout=layout;
        VkPipeline pipeline{}; Check(vkCreateComputePipelines(device,nullptr,1,&ci,nullptr,&pipeline)); ++live; ++created;
        return std::unique_ptr<PipelineLifetimeToken>(new PipelineLifetimeToken{this,pipeline});
    }
    void SubmitBlocked(const PipelineLifetimeToken &token) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; si.pNext=&type; Check(vkCreateSemaphore(device,&si,nullptr,&release));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1; Check(vkAllocateCommandBuffers(device,&ai,&command));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Check(vkBeginCommandBuffer(command,&bi));
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,token.pipeline); vkCmdDispatch(command,1,1,1); Check(vkEndCommandBuffer(command));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; Check(vkCreateFence(device,&fi,nullptr,&fence));
        uint64_t value=1; VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; ti.waitSemaphoreValueCount=1; ti.pWaitSemaphoreValues=&value;
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&ti; submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&release; submit.pWaitDstStageMask=&stage; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        Check(vkQueueSubmit(queue,1,&submit,fence));
        if(vkGetFenceStatus(device,fence)!=VK_NOT_READY) std::exit(2);
    }
    bool Complete() {
        // This fence must still be pending after specialization transitions.
        if(vkGetFenceStatus(device,fence)!=VK_NOT_READY) return false;
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore=release; signal.value=1; Check(vkSignalSemaphore(device,&signal));
        Check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX));
        std::printf("PASS Vulkan pipeline variants: %u pipelines retained while old fence pending, %u validation errors\n",created,errors);
        return errors==0;
    }
    ~PipelineLifetimeRuntime() {
        if(live) { std::fprintf(stderr,"live pipelines at device teardown: %u\n", live); std::abort(); }
        Check(vkDeviceWaitIdle(device));
        vkDestroyFence(device,fence,nullptr); vkDestroySemaphore(device,release,nullptr); vkDestroyCommandPool(device,pool,nullptr); vkDestroyShaderModule(device,module,nullptr); vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDevice(device,nullptr);
        auto destroyDebug=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT")); destroyDebug(instance,debug,nullptr); vkDestroyInstance(instance,nullptr);
        active=nullptr;
    }
};
inline PipelineLifetimeToken::~PipelineLifetimeToken() { vkDestroyPipeline(runtime->device,pipeline,nullptr); --runtime->live; }
