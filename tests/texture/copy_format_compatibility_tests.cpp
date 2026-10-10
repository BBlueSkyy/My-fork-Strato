#include <cassert>
#include <skyline/gpu/texture/copy_format_compatibility.h>

using namespace skyline::gpu::texture;

namespace {
    constexpr auto Color = vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor};
    constexpr auto Depth = vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
}

int main() {
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eR8G8B8A8Unorm, Color,
        vk::Format::eR8G8B8A8Unorm, Color, false) == FormatCompatibility::Exact);

    // A verified host view remains Full-capable rather than being downgraded to a copy.
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eR8G8B8A8Unorm, Color,
        vk::Format::eR8G8B8A8Srgb, Color, true) == FormatCompatibility::ViewCompatible);

    // Compressed numeric variants in the same Vulkan compatibility class are
    // bitwise copy-compatible in both directions.
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eBc1RgbaUnormBlock, Color,
        vk::Format::eBc1RgbaSrgbBlock, Color, false) == FormatCompatibility::CopyCompatible);
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eBc1RgbaSrgbBlock, Color,
        vk::Format::eBc1RgbaUnormBlock, Color, false) == FormatCompatibility::CopyCompatible);

    // Uncompressed color formats use the normative Vulkan compatibility class,
    // not component count or numeric interpretation.
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eR32Uint, Color,
        vk::Format::eR8G8B8A8Unorm, Color, false) == FormatCompatibility::CopyCompatible);
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eR8G8B8A8Unorm, Color,
        vk::Format::eR32Uint, Color, false) == FormatCompatibility::CopyCompatible);

    // Matching byte size is insufficient when the Vulkan classes differ.
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eBc1RgbaUnormBlock, Color,
        vk::Format::eBc4UnormBlock, Color, false) == FormatCompatibility::Incompatible);
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eBc1RgbaUnormBlock, Color,
        vk::Format::eR32G32Uint, Color, false) == FormatCompatibility::Incompatible);

    // Depth/stencil-to-color copies require separate explicit Vulkan rules and
    // are intentionally outside this first CopyCompatible classifier.
    assert(ClassifyHostFormatCompatibility(
        vk::Format::eD32Sfloat, Depth,
        vk::Format::eR32Uint, Color, false) == FormatCompatibility::Incompatible);
}
