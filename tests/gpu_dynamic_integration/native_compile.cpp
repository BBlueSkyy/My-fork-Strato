#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
using u32=uint32_t;
template<class T> using span=std::span<T>;
struct GPU {
    vk::raii::Device vkDevice{nullptr};
    struct { bool supportsPushDescriptors{}; struct { bool brokenMultithreadedPipelineCompilation{}; } quirks; } traits;
    struct Assembler { void WaitIdle(); };
    std::shared_ptr<Assembler> graphicsPipelineAssembler;
};
struct InterconnectContext { GPU &gpu; };
struct DynamicDescriptors {
    void ValidateLimits(InterconnectContext&,u32,u32,u32,u32);
    vk::DescriptorSetLayout Layout();
};
struct PackedPipelineState {};
struct Pipeline {
    struct ShaderStage { vk::ShaderModule module; };
    struct CompiledPipeline {
        vk::raii::DescriptorSetLayout descriptorSetLayout;
        vk::raii::PipelineLayout pipelineLayout;
        vk::raii::Pipeline pipeline;
    };
};
#include "bind.inc"
#include "native_layout.inc"
struct FenceCycle;
struct StateUpdater { void RecordAll(GPU&,vk::raii::CommandBuffer&); };
struct DrawParams { StateUpdater stateUpdater; std::array<u32,3> dimensions; vk::PipelineStageFlags srcStageMask,dstStageMask; bool dynamicDescriptors; };
void NativeDispatch(DrawParams *drawParams) {
#include "dispatch_callback.inc"
    (void)callback;
}
