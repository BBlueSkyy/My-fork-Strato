#pragma once
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

// Test-only dispatch interception: count real Vulkan objects and the driver's
// allocations for those objects. Production receives no counters or callbacks.
namespace test {
    struct Counts {
        std::atomic<uint64_t> sets{}, pipelines{}, setDestroyed{}, pipelineDestroyed{};
        std::atomic<uint64_t> bytes{}, peak{}, allocations{}, descriptorSets{}, pools{};
        std::atomic<int> failSet{}, failPipeline{}, errors{};
        std::atomic<uint32_t> seenSetFlags{}, seenPipelineFlags{};
        std::atomic<const void *> seenPipelineNext{};
        void Reset() {
            if (bytes != 0 || sets != setDestroyed || pipelines != pipelineDestroyed)
                throw std::runtime_error("layout objects or driver allocations leaked");
            sets=0; pipelines=0; setDestroyed=0; pipelineDestroyed=0;
            peak=0; allocations=0; descriptorSets=0; pools=0;
        }
    };
    inline Counts counts;
    struct Allocation { void *base; size_t size; };
    inline void *VKAPI_PTR Allocate(void *, size_t size, size_t alignment, VkSystemAllocationScope) {
        alignment=std::max(alignment,alignof(Allocation));
        auto base=std::malloc(size+alignment+sizeof(Allocation));
        if (!base) return nullptr;
        auto address=(reinterpret_cast<uintptr_t>(base)+sizeof(Allocation)+alignment-1)&~(alignment-1);
        auto header=reinterpret_cast<Allocation *>(address)-1;
        *header={base,size};
        auto now=counts.bytes.fetch_add(size)+size;auto old=counts.peak.load();
        while(old<now&&!counts.peak.compare_exchange_weak(old,now)){}
        counts.allocations++;
        return reinterpret_cast<void *>(address);
    }
    inline void VKAPI_PTR Free(void *, void *ptr) {
        if(!ptr) return;
        auto h=reinterpret_cast<Allocation *>(ptr)-1;
        counts.bytes-=h->size;std::free(h->base);
    }
    inline void *VKAPI_PTR Reallocate(void *u,void *ptr,size_t size,size_t alignment,VkSystemAllocationScope scope) {
        if(!size){Free(u,ptr);return nullptr;}
        auto next=Allocate(u,size,alignment,scope);
        if(next&&ptr){std::memcpy(next,ptr,std::min(size,(reinterpret_cast<Allocation *>(ptr)-1)->size));Free(u,ptr);}
        return next;
    }
    inline const VkAllocationCallbacks allocator{nullptr,Allocate,Reallocate,Free,nullptr,nullptr};
    inline VkResult VKAPI_PTR CreateSet(VkDevice d,const VkDescriptorSetLayoutCreateInfo *i,const VkAllocationCallbacks *,VkDescriptorSetLayout *out) {
        counts.seenSetFlags=i->flags;
        if(counts.failSet.exchange(0)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        auto fn=reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(::vkGetDeviceProcAddr(d,"vkCreateDescriptorSetLayout"));
        auto r=fn(d,i,&allocator,out);if(r==VK_SUCCESS)counts.sets++;return r;
    }
    inline void VKAPI_PTR DestroySet(VkDevice d,VkDescriptorSetLayout s,const VkAllocationCallbacks *) {
        if(s)counts.setDestroyed++;
        reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(::vkGetDeviceProcAddr(d,"vkDestroyDescriptorSetLayout"))(d,s,&allocator);
    }
    inline VkResult VKAPI_PTR CreatePipeline(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *,VkPipelineLayout *out) {
        counts.seenPipelineFlags=i->flags;counts.seenPipelineNext=i->pNext;
        if(counts.failPipeline.exchange(0)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        auto fn=reinterpret_cast<PFN_vkCreatePipelineLayout>(::vkGetDeviceProcAddr(d,"vkCreatePipelineLayout"));
        auto r=fn(d,i,&allocator,out);if(r==VK_SUCCESS)counts.pipelines++;return r;
    }
    inline void VKAPI_PTR DestroyPipeline(VkDevice d,VkPipelineLayout p,const VkAllocationCallbacks *) {
        if(p)counts.pipelineDestroyed++;
        reinterpret_cast<PFN_vkDestroyPipelineLayout>(::vkGetDeviceProcAddr(d,"vkDestroyPipelineLayout"))(d,p,&allocator);
    }
    inline VkResult VKAPI_PTR AllocateSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *out) {
        auto r=reinterpret_cast<PFN_vkAllocateDescriptorSets>(::vkGetDeviceProcAddr(d,"vkAllocateDescriptorSets"))(d,i,out);
        if(r==VK_SUCCESS)counts.descriptorSets+=i->descriptorSetCount;return r;
    }
    inline VkResult VKAPI_PTR CreatePool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *out) {
        auto r=reinterpret_cast<PFN_vkCreateDescriptorPool>(::vkGetDeviceProcAddr(d,"vkCreateDescriptorPool"))(d,i,a,out);
        if(r==VK_SUCCESS)counts.pools++;return r;
    }
    inline PFN_vkVoidFunction Hook(const char *name) {
#define HOOK(n, f) if(!std::strcmp(name,n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
        HOOK("vkCreateDescriptorSetLayout",CreateSet); HOOK("vkDestroyDescriptorSetLayout",DestroySet);
        HOOK("vkCreatePipelineLayout",CreatePipeline); HOOK("vkDestroyPipelineLayout",DestroyPipeline);
        HOOK("vkAllocateDescriptorSets",AllocateSets); HOOK("vkCreateDescriptorPool",CreatePool);
#undef HOOK
        return nullptr;
    }
    inline PFN_vkVoidFunction VKAPI_PTR DeviceProc(VkDevice d,const char *name) {
        if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
        if(auto p=Hook(name))return p;return ::vkGetDeviceProcAddr(d,name);
    }
    inline PFN_vkVoidFunction VKAPI_PTR InstanceProc(VkInstance i,const char *name) {
        if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
        if(auto p=Hook(name))return p;return ::vkGetInstanceProcAddr(i,name);
    }
    inline VkBool32 VKAPI_PTR Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void *) {
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){counts.errors++;std::cerr<<data->pMessage<<"\n";}
        return VK_FALSE;
    }
    inline int checks{};
    inline void Check(bool ok,const char *message) {
        ++checks;if(!ok)throw std::runtime_error(message);
    }
    struct Context {
        vk::raii::Context context;
        vk::raii::Instance instance{nullptr};
        vk::raii::DebugUtilsMessengerEXT debug{nullptr};
        vk::raii::PhysicalDevice physical{nullptr};
        vk::raii::Device device{nullptr};
        uint32_t family{};
        bool push{};
        explicit Context(bool validation=true, bool intercept=true)
            : context{intercept ? InstanceProc : ::vkGetInstanceProcAddr} {
            const char *layer="VK_LAYER_KHRONOS_validation";const char *ext=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            vk::ApplicationInfo app{.pApplicationName="strato-layout-tests",.apiVersion=VK_API_VERSION_1_2};
            vk::ValidationFeatureEnableEXT enable=vk::ValidationFeatureEnableEXT::eSynchronizationValidation;
            vk::ValidationFeaturesEXT vf{.enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&enable};
            instance=vk::raii::Instance{context,vk::InstanceCreateInfo{
                .pNext=validation?&vf:nullptr,.pApplicationInfo=&app,.enabledLayerCount=validation?1U:0U,
                .ppEnabledLayerNames=validation?&layer:nullptr,.enabledExtensionCount=validation?1U:0U,
                .ppEnabledExtensionNames=validation?&ext:nullptr}};
            if(validation)debug=vk::raii::DebugUtilsMessengerEXT{instance,vk::DebugUtilsMessengerCreateInfoEXT{
                .messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
                .messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral|vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation|vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
                .pfnUserCallback=Debug}};
            physical=std::move(vk::raii::PhysicalDevices(instance).front());
            auto families=physical.getQueueFamilyProperties();
            while(!(families.at(family).queueFlags&vk::QueueFlagBits::eGraphics))++family;
            for(const auto &e:physical.enumerateDeviceExtensionProperties())
                if(!std::strcmp(e.extensionName.data(),VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))push=true;
            const char *pushName=VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;float priority=1;
            vk::DeviceQueueCreateInfo qi{.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
            device=vk::raii::Device{physical,vk::DeviceCreateInfo{.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
                .enabledExtensionCount=push?1U:0U,.ppEnabledExtensionNames=push?&pushName:nullptr}};
        }
    };
}
