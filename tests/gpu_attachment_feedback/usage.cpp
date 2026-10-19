// SPDX-License-Identifier: MPL-2.0
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <gpu/attachment_feedback.h>
#include <iostream>
#include <memory>

struct Format { vk::Format vkFormat; };
struct PhysicalDevice {
    vk::FormatFeatureFlags features;
    vk::FormatProperties getFormatProperties(vk::Format) const {
        return {.linearTilingFeatures = features, .optimalTilingFeatures = features};
    }
};
struct Texture {
    struct TextureViewStorage {
        struct Handle {
            vk::ImageView value;
            vk::ImageView operator*() const { return value; }
        } vkView;
    };
    std::vector<TextureViewStorage> views;
    bool OwnsView(vk::ImageView view) const;
    struct { PhysicalDevice vkPhysicalDevice; } gpu;
    vk::ImageUsageFlags usage;
    vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
    Format native{vk::Format::eR8G8B8A8Unorm};
    Format *format{&native};
};
struct TextureView {
    std::shared_ptr<Texture> texture;
    Format *format;
    vk::ImageUsageFlags GetUsage() const;
};
#include "usage.inc"

int main() {
    using namespace skyline::gpu;
    auto texture{std::make_shared<Texture>()};
    Format foreign{vk::Format::eR8G8B8A8Srgb};
    TextureView view{texture, &foreign};
    texture->usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment | feedback::ImageUsageBit;
    texture->gpu.vkPhysicalDevice.features = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eColorAttachment;
    const auto usage{view.GetUsage()};
    if (!(usage & feedback::ImageUsageBit) || (usage & vk::ImageUsageFlagBits::eStorage) ||
        !(usage & vk::ImageUsageFlagBits::eSampled) || !(usage & vk::ImageUsageFlagBits::eColorAttachment))
        throw std::runtime_error("foreign-format usage filtering lost feedback usage or kept unsupported storage");
    texture->gpu.vkPhysicalDevice.features = {};
    if (view.GetUsage() != feedback::ImageUsageBit)
        throw std::runtime_error("individual usage filtering lost an unknown extension bit");
    view.format = texture->format;
    if (view.GetUsage() != texture->usage)
        throw std::runtime_error("native view usage changed");
    const vk::ImageView stored{reinterpret_cast<VkImageView>(uintptr_t{29})};
    const vk::ImageView unrelated{reinterpret_cast<VkImageView>(uintptr_t{31})};
    texture->views.push_back({{stored}});
    if (!texture->OwnsView(stored) || texture->OwnsView(unrelated) || texture->OwnsView({}))
        throw std::runtime_error("feedback storage guard misidentified cached backing views");
    texture->views.clear(); // Backing promotion invalidates the old view cache.
    if (texture->OwnsView(stored))
        throw std::runtime_error("feedback storage guard matched a retired backing view");
    std::cout << "PASS: production view usage preserves extension bits while removing unsupported format usages\n";
}
