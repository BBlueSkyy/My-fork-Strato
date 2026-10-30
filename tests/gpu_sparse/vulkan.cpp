#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_SETTERS
#define VULKAN_HPP_NO_SMART_HANDLE
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <array>
#include <memory>
#include <cassert>
#include <iostream>
using u32 = uint32_t;
namespace skyline::gpu {
namespace texture {
using Dimensions = vk::Extent3D;
struct FormatBase { vk::Format vkFormat; };
struct Format { const FormatBase *value; constexpr Format(const FormatBase &v) : value(&v) {} constexpr const FormatBase *operator->() const { return value; } };
}
namespace format { constexpr texture::FormatBase R8G8B8A8Unorm{vk::Format::eR8G8B8A8Unorm}; }
struct Cycle { template<class T> void AttachObject(T); };
struct Memory {
    struct Image { VmaAllocator vmaAllocator; VmaAllocation vmaAllocation; };
    Image AllocateImage(const vk::ImageCreateInfo &);
};
struct Scheduler { template<class T> std::shared_ptr<Cycle> Submit(T fn) { decltype(fn(std::declval<vk::raii::CommandBuffer &>())) *unused{}; (void)unused; return {}; } };
struct GPU { vk::raii::Device vkDevice{nullptr}; vk::raii::PhysicalDevice vkPhysicalDevice{nullptr}; u32 vkQueueFamilyIndex; Memory memory; Scheduler scheduler; };
struct Texture {
    Texture(GPU &, Memory::Image &&, texture::Dimensions, texture::Format, vk::ImageLayout, vk::ImageTiling, vk::ImageCreateFlags, vk::ImageUsageFlags, u32);
    vk::Image GetBacking();
    std::shared_ptr<Cycle> cycle;
    vk::ImageLayout layout;
};
struct InterconnectContext { GPU &gpu; };
namespace interconnect {
#include "sparse-swizzle.inc"
#ifndef SWIZZLE_ONLY
#include "sparse-vulkan.inc"
#endif
}
}

int main() {
    using vk::ComponentSwizzle;
    using skyline::gpu::interconnect::ConvertSparseZeroSwizzle;
    const vk::ComponentMapping alphaEverywhere{ComponentSwizzle::eA, ComponentSwizzle::eA, ComponentSwizzle::eA, ComponentSwizzle::eA};
    const auto bc5 = ConvertSparseZeroSwizzle(vk::Format::eBc5UnormBlock, alphaEverywhere);
    assert(bc5.r == ComponentSwizzle::eOne && bc5.g == ComponentSwizzle::eOne && bc5.b == ComponentSwizzle::eOne && bc5.a == ComponentSwizzle::eOne);
    const auto r8 = ConvertSparseZeroSwizzle(vk::Format::eR8Unorm, alphaEverywhere);
    assert(r8 == bc5);
    const auto rgba = ConvertSparseZeroSwizzle(vk::Format::eR8G8B8A8Unorm, alphaEverywhere);
    assert(rgba == alphaEverywhere);
    const auto astc = ConvertSparseZeroSwizzle(vk::Format::eAstc8x8UnormBlock, alphaEverywhere);
    assert(astc == alphaEverywhere);
    const vk::ComponentMapping constants{ComponentSwizzle::eZero, ComponentSwizzle::eOne, ComponentSwizzle::eR, ComponentSwizzle::eA};
    const auto converted = ConvertSparseZeroSwizzle(vk::Format::eBc5UnormBlock, constants);
    assert(converted.r == ComponentSwizzle::eZero && converted.g == ComponentSwizzle::eOne && converted.b == ComponentSwizzle::eR && converted.a == ComponentSwizzle::eOne);
    const auto identity = ConvertSparseZeroSwizzle(vk::Format::eR8Unorm, {});
    assert(identity.a == ComponentSwizzle::eOne && identity.r == ComponentSwizzle::eIdentity);
    std::cout << "PASS: format-aware BC5/R8/RGBA/ASTC sparse swizzles and explicit Zero/One\n";
}
