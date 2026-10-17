// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <gpu/buffer_mapping_retry.h>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace skyline::gpu {
    class Texture;
    // Internal control flow: caught before publishing the operation's GPU nodes.
    struct TextureBackingPromotionRequest {
        std::shared_ptr<Texture> texture;
        vk::Format format;
        uint32_t levels{}; // Zero preserves the current mip chain (format-only promotion).
        std::shared_ptr<void> guestMemoryLease{}; // Keep unrepresented guest mip bytes through executor submission.
    };

    namespace texture {
        template<typename Operation, typename Resolve>
        void RetryBackingPromotion(Operation &&operation, Resolve &&resolve) {
            std::vector<Buffer *> reconciledBuffers;
            for (;;) {
                try {
                    operation();
                    return;
                } catch (const TextureBackingPromotionRequest &request) {
                    resolve(request);
                } catch (const BufferMappingReconciliationRequest &request) {
                    if constexpr (requires { resolve(request); }) {
                        if (std::find(reconciledBuffers.begin(), reconciledBuffers.end(), request.target.get()) != reconciledBuffers.end())
                            throw std::runtime_error("Conflicting physical buffer aliases in one GPU operation");
                        reconciledBuffers.push_back(request.target.get());
                        resolve(request);
                    } else throw;
                } catch (const TextureAliasReconciliationRequest &request) {
                    if constexpr (requires { resolve(request); })
                        resolve(request);
                    else
                        throw;
                }
            }
        }

        inline bool ViewFormatAdmitted(vk::Format native, vk::ImageCreateFlags flags,
                                       const std::vector<vk::Format> &formats, vk::Format requested) {
            return requested == native ||
                ((flags & vk::ImageCreateFlagBits::eMutableFormat) &&
                 (formats.empty() || std::find(formats.begin(), formats.end(), requested) != formats.end()));
        }

        inline std::vector<vk::Format> AdmitViewFormat(vk::Format native, std::vector<vk::Format> formats, vk::Format requested) {
            for (auto format : {native, requested})
                if (std::find(formats.begin(), formats.end(), format) == formats.end())
                    formats.push_back(format);
            return formats;
        }

        inline std::vector<vk::ImageCopy> PromotionCopyRegions(vk::Extent3D extent, uint32_t levels, uint32_t layers) {
            std::vector<vk::ImageCopy> copies;
            copies.reserve(levels);
            for (uint32_t level{}; level < levels; ++level) {
                vk::ImageCopy copy{};
                copy.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, layers};
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = extent;
                copies.push_back(copy);
                extent = vk::Extent3D{std::max(1U, extent.width / 2), std::max(1U, extent.height / 2), std::max(1U, extent.depth / 2)};
            }
            return copies;
        }

        template<typename CommandBuffer>
        void RecordBackingPromotion(CommandBuffer &commands, vk::Image source, vk::Image destination,
                                    vk::ImageLayout layout, uint32_t levels, uint32_t layers,
                                    const std::vector<vk::ImageCopy> &copies, uint32_t destinationLevels = 0,
                                    vk::Buffer initialization = {}, const std::vector<vk::BufferImageCopy> &uploads = {}) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, levels, 0, layers};
            std::array<vk::ImageMemoryBarrier, 2> before{};
            before[0].srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            before[0].dstAccessMask = vk::AccessFlagBits::eTransferRead;
            before[0].oldLayout = layout;
            before[0].newLayout = vk::ImageLayout::eTransferSrcOptimal;
            before[0].image = source;
            before[1].dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            before[1].oldLayout = vk::ImageLayout::eUndefined;
            before[1].newLayout = vk::ImageLayout::eTransferDstOptimal;
            before[1].image = destination;
            for (auto &barrier : before) {
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = range;
            }
            before[1].subresourceRange.levelCount = destinationLevels ? destinationLevels : levels;
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
            commands.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, destination, vk::ImageLayout::eTransferDstOptimal, copies);
            // The upload covers only added mips; existing levels come from the GPU
            // image even when their corresponding guest bytes are stale.
            if (!uploads.empty())
                commands.copyBufferToImage(initialization, destination, vk::ImageLayout::eTransferDstOptimal, uploads);
            auto after{before};
            after[0].srcAccessMask = vk::AccessFlagBits::eTransferRead;
            after[1].srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            for (auto &barrier : after) {
                barrier.oldLayout = barrier.newLayout;
                barrier.newLayout = layout;
                barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            }
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
        }
    }
}
