// SPDX-License-Identifier: MPL-2.0
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <boost/functional/hash.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

using u32 = uint32_t;
template<typename T> using span = std::span<T>;
template<typename... T> struct VariantVisitor : T... { using T::operator()...; };
template<typename... T> VariantVisitor(T...) -> VariantVisitor<T...>;
namespace skyline::gpu {
    class GPU;
    namespace cache {
        using FramebufferCreateInfo = vk::StructureChain<vk::FramebufferCreateInfo, vk::FramebufferAttachmentsCreateInfo>;
        #define private public
        #include "class.inc"
        #undef private
        #include "production.inc"
    }
}

int main() {
    using namespace skyline::gpu::cache;
    std::array<vk::Format, 3> formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Uint};
    vk::FramebufferAttachmentImageInfo image{.flags = vk::ImageCreateFlagBits::eMutableFormat,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment,
        .width = 20, .height = 12, .layerCount = 2, .viewFormatCount = 2, .pViewFormats = formats.data()};
    FramebufferCreateInfo createInfo{
        vk::FramebufferCreateInfo{.flags = vk::FramebufferCreateFlagBits::eImageless,
            .attachmentCount = 1, .width = 20, .height = 12, .layers = 2},
        vk::FramebufferAttachmentsCreateInfo{.attachmentImageInfoCount = 1, .pAttachmentImageInfos = &image}};
    FramebufferCache::FramebufferHash hash;
    FramebufferCache::FramebufferEqual equal;
    FramebufferCache::FramebufferCacheKey first{createInfo};
    assert(equal(first, createInfo) && hash(first) == hash(createInfo));
    auto identical{first};
    assert(equal(first, identical) && hash(first) == hash(identical));
    image.viewFormatCount = 3;
    assert(!equal(first, createInfo));
    FramebufferCache::FramebufferCacheKey expanded{createInfo};
    assert(equal(expanded, createInfo) && hash(expanded) == hash(createInfo));
    assert(!equal(first, expanded));
    image.viewFormatCount = 2;
    formats[1] = vk::Format::eR8G8B8A8Sint;
    assert(!equal(first, createInfo)); // Same first format/count is insufficient for reuse.
    FramebufferCache::FramebufferCacheKey changed{createInfo};
    assert(equal(changed, createInfo) && hash(changed) == hash(createInfo));
    assert(std::get<std::vector<FramebufferCache::FramebufferImagelessAttachment>>(first.attachments).front().formats[1]
        == vk::Format::eB8G8R8A8Unorm); // Cache owns the complete list, not caller pointers.

    createInfo.unlink<vk::FramebufferAttachmentsCreateInfo>();
    auto &fixed{createInfo.get<vk::FramebufferCreateInfo>()};
    fixed.flags = {};
    const vk::ImageView view{reinterpret_cast<VkImageView>(uintptr_t{0x100})};
    fixed.pAttachments = &view;
    FramebufferCache::FramebufferCacheKey key{createInfo};
    assert(equal(key, createInfo) && hash(key) == hash(createInfo));
    std::puts("Production framebuffer cache: full format-list ownership, expansion, heterogeneous hash/equality and fixed-view keys passed");
}
