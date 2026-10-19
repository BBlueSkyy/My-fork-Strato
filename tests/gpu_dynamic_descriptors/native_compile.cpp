// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <cstdint>
#include <compare>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include "../../app/src/main/cpp/skyline/gpu/attachment_feedback.h"
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
namespace Shader {
enum class TextureType { Color2D };
enum class TextureComponentType { Float, Uint, Sint };
#include "dynamic_shader.inc"
}
#define LOGE(...) do {} while(false)
namespace skyline {
template<class T> using span = std::span<T>;
struct exception : std::runtime_error { template<class... Args> exception(const char *s, Args&&...) : std::runtime_error(s) {} };
namespace gpu {
struct FenceCycle { bool Poll(bool, bool); };
namespace memory {
struct Buffer { vk::Buffer vkBuffer; u8 *data(); };
struct MemoryManager { Buffer AllocateBuffer(size_t); };
}
struct GPU { vk::raii::Device vkDevice{nullptr}; memory::MemoryManager memory; struct { u32 maximumStorageBufferRange; vk::PhysicalDeviceLimits dynamicDescriptorLimits; } traits; };
struct Texture {
 u64 backingGeneration{1};
 u64 GetBackingGeneration() const { return backingGeneration; }
 vk::ImageLayout layout=vk::ImageLayout::eUndefined;
 void TransitionLayout(vk::ImageLayout);
 void PopulateReadBarrier(vk::PipelineStageFlagBits, vk::PipelineStageFlags &, vk::PipelineStageFlags &);
};
struct TextureView : std::enable_shared_from_this<TextureView> {
 std::shared_ptr<Texture> texture;
 vk::ImageUsageFlags GetUsage();
 vk::ImageView GetView();
};
namespace interconnect {
struct CommandExecutor { std::shared_ptr<FenceCycle> cycle; void AttachTexture(TextureView *); void AttachDependency(const std::shared_ptr<void> &); };
struct InterconnectContext { GPU &gpu; CommandExecutor &executor; };
using TextureImageControl = u32;
using TextureSamplerControl = u32;
struct Textures {
#include "textures_api.inc"
};
struct Samplers {
#include "samplers_api.inc"
};
}
}
}
#include "header.inc"
#include "source.inc"
