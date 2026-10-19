// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
using u64 = uint64_t;
struct Device {
    std::vector<vk::ImageViewCreateInfo> created;
};
namespace vk::raii {
// Only Vulkan object creation is replaced; all view selection/cache code is production.
struct ImageView {
    vk::ImageView handle;
    ImageView(::Device &device, const vk::ImageViewCreateInfo &info) {
        device.created.push_back(info);
        handle = vk::ImageView{reinterpret_cast<VkImageView>(device.created.size())};
    }
    vk::ImageView operator*() const { return handle; }
};
}
struct Format {
    vk::Format vkFormat{vk::Format::eR16G16Uint};
    vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
    operator vk::Format() const { return vkFormat; }
};
struct GPU { Device vkDevice; };
struct Texture {
    struct TextureViewStorage {
        vk::ImageViewType type;
        Format *format;
        vk::ComponentMapping mapping;
        vk::ImageSubresourceRange range;
        vk::raii::ImageView vkView;
        TextureViewStorage(vk::ImageViewType type, Format *format, vk::ComponentMapping mapping,
                           vk::ImageSubresourceRange range, vk::raii::ImageView &&view)
            : type{type}, format{format}, mapping{mapping}, range{range}, vkView{std::move(view)} {}
    };
    GPU gpu;
    Format native;
    Format *format{&native};
    std::vector<TextureViewStorage> views;
    u64 backingGeneration{1};
    bool ViewRangeFits(vk::ImageViewType, vk::ImageSubresourceRange) const { return true; }
    bool ViewFormatAdmitted(vk::Format) const { return true; }
    vk::Image GetBacking() const { return vk::Image{reinterpret_cast<VkImage>(backingGeneration)}; }
};
template<class... Args> std::runtime_error exception(const char *message, const Args &...) { return std::runtime_error(message); }
struct TextureView {
    vk::ImageView vkView{}, vkStorageView{};
    u64 vkViewGeneration{}, vkStorageViewGeneration{};
    std::shared_ptr<Texture> texture;
    vk::ImageViewType type{vk::ImageViewType::e2D};
    Format *format;
    vk::ComponentMapping mapping;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageUsageFlags GetUsage() const { return vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage; }
    vk::ImageView GetView(bool identityMapping = false);
};
#include "production.inc"
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        for (bool storageFirst : {false, true}) {
            auto texture{std::make_shared<Texture>()};
            TextureView view{.texture = texture, .format = texture->format,
                .mapping = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG,
                            vk::ComponentSwizzle::eZero, vk::ComponentSwizzle::eOne}};
            view.GetView(storageFirst);
            const auto sampled{view.GetView()};
            const auto storage{view.GetView(true)};
            check(sampled != storage, "storage view reused a sampled TIC swizzle");
            check(texture->views.size() == 2, "view mappings must occupy separate cache entries");
            const auto &sampledInfo{texture->gpu.vkDevice.created[storageFirst ? 1 : 0]};
            const auto &storageInfo{texture->gpu.vkDevice.created[storageFirst ? 0 : 1]};
            check(sampledInfo.components == view.mapping, "sampled TIC swizzle changed");
            check(storageInfo.components == vk::ComponentMapping{}, "storage components are not identity");
            check(storageInfo.image == sampledInfo.image && storageInfo.format == sampledInfo.format &&
                  storageInfo.subresourceRange == sampledInfo.subresourceRange, "identity view changed resource semantics");
            for (int i = 0; i < 100; ++i)
                check(view.GetView() == sampled && view.GetView(true) == storage, "hot view cache changed handles");
            check(texture->gpu.vkDevice.created.size() == 2, "hot path recreated an image view");
            TextureView other{.texture = texture, .format = texture->format, .mapping = view.mapping};
            check(other.GetView() == sampled && other.GetView(true) == storage, "backing view cache did not share handles");
            texture->views.clear();
            ++texture->backingGeneration;
            check(view.GetView() != sampled && view.GetView(true) != storage, "backing swap left a stale cached handle");
            check(texture->gpu.vkDevice.created.size() == 4, "both view purposes must recreate exactly once");
            other.mapping = vk::ComponentMapping{};
            check(other.GetView(true) == view.GetView(true), "identity storage cache depends on TIC mapping");
        }
        std::cout << "PASS production sampled/storage image views, identity mapping, caches and backing generations\n";
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
