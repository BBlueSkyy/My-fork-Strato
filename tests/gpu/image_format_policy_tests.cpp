// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <initializer_list>
#include <gpu/texture/image_format_policy.h>
#include <gpu/texture/backing_promotion.h>

int main() {
    using namespace skyline::gpu::texture;
    // A native image needs no mutable permission until a foreign view is requested.
    for (auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
                        vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
                        vk::Format::eR8G8B8A8Uint, vk::Format::eB8G8R8A8Sint,
                        vk::Format::eA8B8G8R8UnormPack32}) {
        assert(!GuestImageFormatFlags(format, true));
        assert(GuestImageFormatFlags(format, false) & vk::ImageCreateFlagBits::eMutableFormat);
    }
    // Preserve the existing BC1/BC3 Unorm/sRGB view guarantee.
    for (auto format : {vk::Format::eBc1RgbaUnormBlock, vk::Format::eBc1RgbaSrgbBlock,
                        vk::Format::eBc3UnormBlock, vk::Format::eBc3SrgbBlock})
        assert(GuestImageFormatFlags(format, true) & vk::ImageCreateFlagBits::eMutableFormat);
    // Unrelated formats retain their existing allocation policy.
    for (auto format : {vk::Format::eR8Unorm, vk::Format::eR16Sfloat,
                        vk::Format::eD24UnormS8Uint, vk::Format::eBc7UnormBlock}) {
        assert(!GuestImageFormatFlags(format, true));
        assert(GuestImageFormatFlags(format, false) & vk::ImageCreateFlagBits::eMutableFormat);
    }
    assert(GuestImageFormatFlags(vk::Format::eR8G8B8A8Unorm, true, false) & vk::ImageCreateFlagBits::eMutableFormat);
    // Depth and stencil formats cannot legally be reinterpreted as foreign
    // formats. They keep their existing native-format view support.
    for (auto format : {vk::Format::eD16Unorm, vk::Format::eD24UnormS8Uint,
                        vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint,
                        vk::Format::eS8Uint}) {
        assert(!GuestImageFormatFlags(format, true, false, false));
        assert(!GuestImageFormatFlags(format, false, false, false));
    }
    // Non-depth compressed color aliases are still allocated mutable.
    assert(GuestImageFormatFlags(vk::Format::eBc7UnormBlock, true, false, true) & vk::ImageCreateFlagBits::eMutableFormat);
    const auto rgba{vk::Format::eR8G8B8A8Unorm}, bgra{vk::Format::eB8G8R8A8Unorm};
    const auto flags{GuestImageFormatFlags(rgba, true)};
    assert(ViewFormatAdmitted(rgba, flags, {}, rgba));
    assert(!ViewFormatAdmitted(rgba, flags, {}, bgra));
    auto list{AdmitViewFormat(rgba, {}, bgra)};
    assert(list.size() == 2);
    const auto promoted{flags | vk::ImageCreateFlagBits::eMutableFormat};
    assert(ViewFormatAdmitted(rgba, promoted, list, bgra));
    assert(AdmitViewFormat(rgba, list, bgra) == list);
    const auto third{vk::Format::eR8G8B8A8Uint};
    assert(!ViewFormatAdmitted(rgba, promoted, list, third));
    assert(ViewFormatAdmitted(rgba, promoted, {}, third)); // No extension: unrestricted mutable backing.
    list = AdmitViewFormat(rgba, list, third);
    assert(list.size() == 3 && ViewFormatAdmitted(rgba, promoted, list, third));

    auto copies{PromotionCopyRegions({20, 12, 1}, 3, 2)};
    assert(copies.size() == 3);
    assert(copies[0].extent == vk::Extent3D(20, 12, 1));
    assert(copies[1].extent == vk::Extent3D(10, 6, 1));
    assert(copies[2].extent == vk::Extent3D(5, 3, 1));
    for (unsigned i{}; i < 3; ++i) {
        assert(copies[i].srcSubresource.mipLevel == i && copies[i].dstSubresource.mipLevel == i);
        assert(copies[i].srcSubresource.layerCount == 2 && copies[i].dstSubresource.layerCount == 2);
    }
    unsigned attempts{}, resolved{}, published{};
    RetryBackingPromotion([&] {
        ++attempts;
        if (resolved < 2) throw skyline::gpu::TextureBackingPromotionRequest{{}, resolved ? rgba : bgra};
        ++published;
    }, [&](const auto &) { ++resolved; });
    assert(attempts == 3 && resolved == 2 && published == 1);
}
