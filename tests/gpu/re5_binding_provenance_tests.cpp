// SPDX-License-Identifier: MPL-2.0
#include <gpu/diagnostic/re5_descriptor_trace.h>
#include <gpu/diagnostic/re5_blit_trace.h>
#include <cassert>
#include <span>

int main() {
    using namespace skyline::gpu::interconnect;
    struct Copy { unsigned srcBinding, srcArrayElement, dstBinding, dstArrayElement, descriptorCount; };
    vk::DescriptorImageInfo actual{.imageView = vk::ImageView{reinterpret_cast<VkImageView>(0x1234)},
                                   .imageLayout = vk::ImageLayout::eGeneral};
    DiagnosticImageBinding binding{.info = &actual, .binding = 7, .element = 2, .stage = 4,
        .viewBackingAtWrite = vk::Image{reinterpret_cast<VkImage>(0xABC)}};
    std::vector<DiagnosticImageBinding> retained;
    skyline::gpu::diagnostic::UpdateImageProvenance(retained, std::span(&binding, 1), std::vector<Copy>{}, 64);
    skyline::gpu::diagnostic::UpdateImageProvenance(retained, std::span<DiagnosticImageBinding>{},
        std::vector<Copy>{{7, 0, 7, 0, 3}}, 64);
    assert(retained.size() == 1 && retained.front().info == &actual);
    actual.imageView = vk::ImageView{reinterpret_cast<VkImageView>(0x5678)};
    DiagnosticBindingProbe probe;
    probe.images = retained;
    probe.RecordConsumedDescriptors();
    assert(probe.recorded);
    assert(probe.result.find("binding=7 array_element=2 stage=4") != std::string::npos);
    assert(probe.result.find("vk_image_view=0x5678") != std::string::npos);
    assert(probe.result.find("view_creation_backing_at_write=0xABC") != std::string::npos);
    assert(probe.result.find("0x1234") == std::string::npos);
    skyline::gpu::diagnostic::UpdateImageProvenance(retained, std::span<DiagnosticImageBinding>{},
        std::vector<Copy>{{7, 0, 8, 0, 3}}, 64);
    assert(retained.empty());
}
