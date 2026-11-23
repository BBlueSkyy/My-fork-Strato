// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <stdexcept>
#include <utility>
#include <gpu/texture/physical_authority_trace.h> // Default-off hooks in extracted bindings.
#include <gpu/interconnect/common/null_texture_policy.h>

// Execute the production binder with real Vulkan sampler handles. Guest TIC
// decoding is exercised by tests.cpp; here its InvalidDescriptor result is the
// boundary input. The resulting descriptor is used for real Vulkan dispatches.
namespace binding_fixture {
using u32 = uint32_t;
using namespace skyline::gpu::interconnect;
namespace vk {
using Sampler = VkSampler;
using PipelineStageFlags = VkPipelineStageFlags;
enum class PipelineStageFlagBits { eComputeShader };
enum class ImageLayout { eShaderReadOnlyOptimal = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
struct DescriptorImageInfo { VkSampler sampler{}; VkImageView imageView{}; ImageLayout imageLayout{}; };
}
struct Resource {
    vk::ImageLayout layout{vk::ImageLayout::eShaderReadOnlyOptimal};
    void PopulateReadBarrier(vk::PipelineStageFlagBits, vk::PipelineStageFlags&, vk::PipelineStageFlags&) {}
};
struct TextureView {
    Resource *texture{};
    VkImageView GetView() { throw std::runtime_error("unexpected guest image path"); }
};
struct InterconnectContext {
    struct { struct { bool supportsNullDescriptor{true}; } traits; } gpu;
    struct { void AttachTexture(TextureView*) { throw std::runtime_error("unexpected texture attachment"); } } executor;
};
struct Textures {
    TextureView *GetTexture(InterconnectContext&, u32, int, NullTextureReason *reason) {
        *reason = NullTextureReason::InvalidDescriptor;
        return nullptr;
    }
};
struct Samplers {
    struct Sampler { VkSampler handle; VkSampler operator*() const { return handle; } } sampler;
    Sampler *GetSampler(InterconnectContext&, u32 index, u32) { return index < 1 ? &sampler : nullptr; }
    Sampler *GetNullSampler(InterconnectContext&) { return &sampler; }
};
inline std::runtime_error exception(const char *message) { return std::runtime_error(message); }
#include "sampled-binding.inc"
inline VkDescriptorImageInfo InvalidTicBinding(VkSampler sampler) {
    InterconnectContext ctx;
    Textures textures;
    Samplers samplers{{sampler}};
    struct { int type{}; } desc;
    vk::PipelineStageFlags src{}, dst{};
    auto result = GetTextureBinding(ctx, desc, samplers, textures, {.raw = 0xFFE00000U},
                                   vk::PipelineStageFlagBits::eComputeShader, src, dst);
    if (result.second) throw std::runtime_error("invalid TIC acquired a guest view");
    return {result.first.sampler, result.first.imageView, static_cast<VkImageLayout>(result.first.imageLayout)};
}
}
