// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <initializer_list>
#include <gpu/texture/image_format_policy.h>

int main() {
    using namespace skyline::gpu::texture;
    // A costly-format hint cannot make a BGRA view of RGBA storage legal.
    for (auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
                        vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb,
                        vk::Format::eR8G8B8A8Uint, vk::Format::eB8G8R8A8Sint,
                        vk::Format::eA8B8G8R8UnormPack32}) {
        assert(GuestImageFormatFlags(format, true) & vk::ImageCreateFlagBits::eMutableFormat);
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
}
