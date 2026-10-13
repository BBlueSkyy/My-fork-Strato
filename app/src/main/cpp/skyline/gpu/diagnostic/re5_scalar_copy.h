// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include "re5_scalar_probe.h"

namespace skyline::gpu::diagnostic {
    // Called only outside a render pass. Caller owns source/destination lifetimes
    // and must wait for its fence and invalidate noncoherent destination memory.
    inline std::optional<vk::DeviceSize> RecordScalarReadback(
        vk::raii::CommandBuffer &commandBuffer, const vk::DescriptorBufferInfo &desc,
        std::uint32_t byteOffset, vk::Buffer destinationBuffer) {
        if (!desc.buffer || !destinationBuffer || desc.range == VK_WHOLE_SIZE ||
            desc.offset % 4 || byteOffset % 4 ||
            !ScalarRangeValid(desc.range, byteOffset) ||
            desc.offset > std::numeric_limits<vk::DeviceSize>::max() - byteOffset - 4)
            return std::nullopt;
        const auto offset{desc.offset + byteOffset};
        // Only the probed four bytes acquire transfer visibility.
        // This is after all consumers in the pass, so it cannot repair
        // a missing dependency for the draw being diagnosed.
        vk::BufferMemoryBarrier source{
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = desc.buffer, .offset = offset, .size = 4,
        };
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
            vk::PipelineStageFlagBits::eTransfer, {}, {}, source, {});
        commandBuffer.copyBuffer(desc.buffer, destinationBuffer, vk::BufferCopy{offset, 0, 4});
        // Order a possible subsequent write after this diagnostic read.
        source.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        source.dstAccessMask = vk::AccessFlagBits::eMemoryWrite;
        vk::BufferMemoryBarrier destination{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = destinationBuffer, .offset = 0, .size = 4,
        };
        const std::array barriers{source, destination};
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
            {}, {}, barriers, {});
        return offset;
    }
}
