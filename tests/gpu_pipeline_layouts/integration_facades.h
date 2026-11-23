#pragma once
#include "context.h"
#include <cstdint>
#include <list>
#include <span>
#include <string>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

// Only platform/application services are facades. The generated includes below
// compile production ownership structs, graphics layout/async creation, compute
// creation, and DescriptorAllocator bodies against real Vulkan-Hpp and Vulkan.
namespace skyline {
    using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
    template<class T> using span=std::span<T>;
    namespace gpu { class GPU; }
    namespace soc::gm20b::engine::maxwell3d::type {}
}
#include <gpu/graphics_pipeline_assembler.h>
namespace skyline::gpu {
    using SpinLock=std::mutex;
    struct TestJvm { void UpdateShaderCompilationState(bool) {} };
    class GPU {
      public:
        const vk::raii::Device &vkDevice;
        PipelineLayoutCache pipelineLayouts;
        struct { struct { bool brokenMultithreadedPipelineCompilation{}; } quirks;
            bool push{};
            bool CanUsePushDescriptors(std::span<const vk::DescriptorSetLayoutBinding>) const { return push; }
        } traits;
        struct { TestJvm *jvm; } state;
        GraphicsPipelineAssembler *graphicsPipelineAssembler{};
        GPU(const vk::raii::Device &device,TestJvm &jvm) : vkDevice{device},pipelineLayouts{device},state{&jvm} {}
    };
}
#include "descriptor-header.inc"
#include "descriptor-source.inc"
namespace skyline::gpu {
    GraphicsPipelineAssembler::GraphicsPipelineAssembler(GPU &gpu,std::string_view path)
        : gpu{gpu},vkPipelineCache{gpu.vkDevice,vk::PipelineCacheCreateInfo{}},pool{2},pipelineCacheDir{path} {}
}
#include "graphics-source.inc"
namespace skyline::gpu::interconnect::kepler_compute {
    struct PackedPipelineState {};
    struct InterconnectContext { GPU &gpu; };
    struct Pipeline {
        struct ShaderStage { vk::ShaderModule module; };
#include "compute-compiled.inc"
    };
#include "compute-source.inc"
}

#include "helper-calls.inc"
