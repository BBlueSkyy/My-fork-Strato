// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <vulkan/vulkan.hpp>
#include <fmt/format.h>

namespace skyline::gpu { class TextureView; }
namespace skyline::gpu::interconnect {
    struct DiagnosticImageBinding {
        const vk::DescriptorImageInfo *info{};
        TextureView *view{};
        std::uint32_t binding{}, element{}, stage{}, rawHandle{}, cbufIndex{};
        std::uint64_t cbufOffset{};
        vk::PipelineStageFlags aggregateSrcStages{}, aggregateDstStages{};
        vk::Image viewBackingAtWrite{};
    };
    struct DiagnosticBindingProbe {
        std::uint32_t index{};
        std::string directory, metadata, result;
        std::vector<DiagnosticImageBinding> images;
        bool recorded{};
        void RecordConsumedDescriptors() {
            result = "consumer_status=vkCmdDraw_recorded_not_shader_execution_trace\n";
            for (const auto &image : images) {
                if (!image.info) continue;
                const auto &desc{*image.info};
                result += fmt::format("CONSUMED binding={} array_element={} stage={} vk_image_view=0x{:X} sampler=0x{:X} descriptor_layout={} view_creation_backing_at_write=0x{:X}\n",
                    image.binding, image.element, image.stage,
                    reinterpret_cast<uintptr_t>(static_cast<VkImageView>(desc.imageView)),
                    reinterpret_cast<uintptr_t>(static_cast<VkSampler>(desc.sampler)), vk::to_string(desc.imageLayout),
                    reinterpret_cast<uintptr_t>(static_cast<VkImage>(image.viewBackingAtWrite)));
            }
            recorded = true;
        }
    };

}
