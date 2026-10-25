// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <kernel/memory.h>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include <common/settings.h>
#include "texture.h"
#include "layout.h"
#include "pitch_linear.h"
#include "adreno_aliasing.h"
#include "bc_decoder.h"
#include "format.h"
#include "image_format_policy.h"
#include "backing_promotion.h"
#include "view_range.h"
#include <bit>
#include <numeric>

namespace skyline::gpu {
    u32 GuestTexture::GetLayerStride() {
        if (layerStride) {
            if (tileConfig.mode == texture::TileMode::Pitch && layerStride < CalculateLayerSize())
                throw std::invalid_argument("Pitch-linear layer stride is smaller than its layer size");
            return layerStride;
        }

        layerStride = CalculateLayerSize();
        return layerStride;
    }

    u32 GuestTexture::CalculateLayerSize() const {
        switch (tileConfig.mode) {
            case texture::TileMode::Linear:
                return static_cast<u32>(format->GetSize(dimensions));

            case texture::TileMode::Pitch:
                // Pitch-linear synchronization supports one 2D mip per layer.
                return texture::GetPitchLinearLayout(dimensions.width, dimensions.height,
                    format->blockWidth, format->blockHeight, format->bpb, tileConfig.pitch, dimensions.depth, mipLevelCount).layerSize;

            case texture::TileMode::Block:
                return static_cast<u32>(texture::GetBlockLinearLayerSize(dimensions, format->blockHeight, format->blockWidth, format->bpb, tileConfig.blockHeight, tileConfig.blockDepth, mipLevelCount, layerCount > 1, tileConfig.tileWidthSpacing));
        }
    }

    vk::ImageType GuestTexture::GetImageType() const {
        switch (viewType) {
            case vk::ImageViewType::e1D:
            case vk::ImageViewType::e1DArray:
                return vk::ImageType::e1D;
            case vk::ImageViewType::e2D:
            case vk::ImageViewType::e2DArray:
                // If depth is > 1 this is a 2D view into a 3D texture so the underlying image needs to be created as 3D yoo
                if (dimensions.depth > 1)
                    return vk::ImageType::e3D;
                else
                    return vk::ImageType::e2D;
            case vk::ImageViewType::eCube:
            case vk::ImageViewType::eCubeArray:
                return vk::ImageType::e2D;
            case vk::ImageViewType::e3D:
                return vk::ImageType::e3D;
        }
    }

    u32 GuestTexture::GetViewLayerCount() const {
        if (GetImageType() == vk::ImageType::e3D && viewType != vk::ImageViewType::e3D)
            return dimensions.depth;
        else
            return layerCount;
    }

    u32 GuestTexture::GetViewDepth() const {
        if (GetImageType() == vk::ImageType::e3D && viewType != vk::ImageViewType::e3D)
            return layerCount;
        else
            return dimensions.depth;
    }

    size_t GuestTexture::GetSize() {
        if (tileConfig.mode == texture::TileMode::Pitch)
            return texture::PitchLinearTextureSize(CalculateLayerSize(), GetLayerStride(), layerCount, baseArrayLayer);
        return GetLayerStride() * (layerCount - baseArrayLayer);
    }

    bool GuestTexture::MappingsValid() const {
        return ranges::all_of(mappings, [](const auto &mapping) { return mapping.valid(); });
    }

    TextureView::TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format, vk::ComponentMapping mapping) : texture(std::move(texture)), type(type), format(format), mapping(mapping), range(range) {}

    Texture::TextureViewStorage::TextureViewStorage(vk::ImageViewType type, texture::Format format, vk::ComponentMapping mapping, vk::ImageSubresourceRange range, vk::raii::ImageView &&vkView) : type(type), format(format), mapping(mapping), range(range), vkView(std::move(vkView)) {}

    vk::ImageUsageFlags TextureView::GetUsage() const {
        auto usage{texture->usage};
        if (!format || format->vkFormat == texture->format->vkFormat) return usage;
        const auto properties{texture->gpu.vkPhysicalDevice.getFormatProperties(format->vkFormat)};
        const auto features{texture->tiling == vk::ImageTiling::eOptimal ? properties.optimalTilingFeatures : properties.linearTilingFeatures};
        if (!(features & vk::FormatFeatureFlagBits::eSampledImage)) usage &= ~vk::ImageUsageFlagBits::eSampled;
        if (!(features & vk::FormatFeatureFlagBits::eStorageImage)) usage &= ~vk::ImageUsageFlagBits::eStorage;
        if (!(features & vk::FormatFeatureFlagBits::eColorAttachment)) usage &= ~vk::ImageUsageFlagBits::eColorAttachment;
        if (!(features & vk::FormatFeatureFlagBits::eDepthStencilAttachment)) usage &= ~vk::ImageUsageFlagBits::eDepthStencilAttachment;
        if (!(features & (vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eDepthStencilAttachment)))
            usage &= ~vk::ImageUsageFlagBits::eInputAttachment;
        return usage;
    }

    vk::ImageView TextureView::GetView(bool identityMapping) {
        if (!texture->ViewRangeFits(type, range) || !range.aspectMask || (range.aspectMask & ~texture->format->vkAspect))
            throw exception("Texture view exceeds backing mip/layer bounds or has an incompatible view type");
        const auto requested{format ? format->vkFormat : texture->format->vkFormat};
        if (!texture->ViewFormatAdmitted(requested))
            throw exception("Texture view format {} is not admitted by backing {}", vk::to_string(requested), vk::to_string(texture->format->vkFormat));
        auto &cachedView{identityMapping ? identityVkView : vkView};
        auto &cachedGeneration{identityMapping ? identityVkViewGeneration : vkViewGeneration};
        if (cachedView && cachedGeneration == texture->backingGeneration)
            return cachedView;

        const vk::ComponentMapping viewMapping{identityMapping ? vk::ComponentMapping{} : mapping};

        auto it{std::find_if(texture->views.begin(), texture->views.end(), [this, &viewMapping](const Texture::TextureViewStorage &view) {
            return view.type == type && view.format == format && view.mapping == viewMapping && view.range == range;
        })};
        if (it == texture->views.end()) {
            // Foreign formats need not support every usage of the native image (e.g.
            // an sRGB sampled alias of a storage-capable UNORM image).
            vk::ImageViewUsageCreateInfo viewUsage{.usage = GetUsage()};
            vk::ImageViewCreateInfo createInfo{
                .pNext = &viewUsage,
                .image = texture->GetBacking(),
                .viewType = type,
                .format = format ? *format : *texture->format,
                .components = viewMapping,
                .subresourceRange = range,
            };

            it = texture->views.emplace(texture->views.end(), type, format, viewMapping, range, vk::raii::ImageView{texture->gpu.vkDevice, createInfo});
        }

        cachedGeneration = texture->backingGeneration;
        return cachedView = *it->vkView;
    }

    void TextureView::lock() {
        texture.Lock();
    }

    bool TextureView::LockWithTag(ContextTag tag) {
        bool result{};
        texture.Lock([tag, &result](Texture *pTexture) {
            result = pTexture->LockWithTag(tag);
        });
        return result;
    }

    void TextureView::unlock() {
        texture->unlock();
    }

    bool TextureView::try_lock() {
        return texture.TryLock();
    }

    void Texture::SetupGuestMappings() {
        auto &mappings{guest->mappings};
        if (mappings.size() == 1) {
            auto mapping{mappings.front()};
            u8 *alignedData{util::AlignDown(mapping.data(), constant::PageSize)};
            size_t alignedSize{static_cast<size_t>(util::AlignUp(mapping.data() + mapping.size(), constant::PageSize) - alignedData)};

            alignedMirror = gpu.state.process->memory.CreateMirror(span<u8>{alignedData, alignedSize});
            mirror = alignedMirror.subspan(static_cast<size_t>(mapping.data() - alignedData), mapping.size());
        } else {
            std::vector<span<u8>> alignedMappings;

            const auto &frontMapping{mappings.front()};
            u8 *alignedData{util::AlignDown(frontMapping.data(), constant::PageSize)};
            alignedMappings.emplace_back(alignedData, (frontMapping.data() + frontMapping.size()) - alignedData);

            size_t totalSize{frontMapping.size()};
            for (auto it{std::next(mappings.begin())}; it != std::prev(mappings.end()); ++it) {
                auto mappingSize{it->size()};
                alignedMappings.emplace_back(it->data(), mappingSize);
                totalSize += mappingSize;
            }

            const auto &backMapping{mappings.back()};
            totalSize += backMapping.size();
            alignedMappings.emplace_back(backMapping.data(), util::AlignUp(backMapping.size(), constant::PageSize));

            alignedMirror = gpu.state.process->memory.CreateMirrors(alignedMappings);
            mirror = alignedMirror.subspan(static_cast<size_t>(frontMapping.data() - alignedData), totalSize);
        }

        // We can't just capture `this` in the lambda since the lambda could exceed the lifetime of the buffer
        std::weak_ptr<Texture> weakThis{weak_from_this()};
        trapHandle = gpu.state.process->trap.CreateTrap(mappings, [weakThis] {
            auto texture{weakThis.lock()};
            if (!texture || texture->aliasStale.load())
                return;

            std::unique_lock stateLock{texture->stateMutex};
            if (texture->dirtyState == DirtyState::GpuDirty) {
                stateLock.unlock(); // If the lock isn't unlocked, a deadlock from threads waiting on the other lock can occur

                // If this mutex would cause other callbacks to be blocked then we should block on this mutex in advance
                std::shared_ptr<FenceCycle> waitCycle{};
                do {
                    // We need to do a loop here since we can't wait with the texture locked but not doing so means that the texture could have it's cycle changed which we wouldn't wait on, loop until we are sure the cycle hasn't changed to avoid that
                    if (waitCycle) {
                        i64 startNs{texture->accumulatedGuestWaitCounter > SkipReadbackHackWaitCountThreshold ? util::GetTimeNs() : 0};
                        waitCycle->Wait();
                        if (startNs)
                            texture->accumulatedGuestWaitTime += std::chrono::nanoseconds(util::GetTimeNs() - startNs);

                        texture->accumulatedGuestWaitCounter++;
                    }

                    std::scoped_lock lock{*texture};
                    if (waitCycle && texture->cycle == waitCycle) {
                        texture->cycle = {};
                        waitCycle = {};
                    } else {
                        waitCycle = texture->cycle;
                    }
                } while (waitCycle);
            }
        }, [weakThis] {
            TRACE_EVENT("gpu", "Texture::ReadTrap");

            auto texture{weakThis.lock()};
            if (!texture)
                return true;

            std::unique_lock stateLock{texture->stateMutex, std::try_to_lock};
            if (!stateLock)
                return false;

            if (texture->aliasStale.load() || texture->dirtyState != DirtyState::GpuDirty)
                return true; // If state is already CPU dirty/Clean we don't need to do anything

            std::unique_lock lock{*texture, std::try_to_lock};
            if (!lock)
                return false;

            if (texture->cycle)
                return false;

            auto group{texture->aliasGroup.load()};
            std::unique_lock<std::recursive_mutex> aliasLock;
            if (group) {
                aliasLock = std::unique_lock{group->operationMutex, std::try_to_lock};
                if (!aliasLock) return false; // trapMutex must never wait on an alias operation.
            }
            texture->SynchronizeGuest(false, true); // We can skip trapping since the caller will do it
            return true;
        }, [weakThis] {
            TRACE_EVENT("gpu", "Texture::WriteTrap");

            auto texture{weakThis.lock()};
            if (!texture)
                return true;

            std::unique_lock stateLock{texture->stateMutex, std::try_to_lock};
            if (!stateLock)
                return false;

            if (texture->aliasStale.load() || texture->dirtyState != DirtyState::GpuDirty) {
                texture->dirtyState = DirtyState::CpuDirty;
                return true; // If the texture is already CPU dirty or we can transition it to being CPU dirty then we don't need to do anything
            }

            if (!texture->HasAliases() && texture->accumulatedGuestWaitTime > SkipReadbackHackWaitTimeThreshold && *texture->gpu.state.settings->enableFastGpuReadbackHack && !texture->memoryFreed) {
                texture->dirtyState = DirtyState::Clean;
                return true;
            }

            std::unique_lock lock{*texture, std::try_to_lock};
            if (!lock)
                return false;

            if (texture->cycle)
                return false;

            auto group{texture->aliasGroup.load()};
            std::unique_lock<std::recursive_mutex> aliasLock;
            if (group) {
                aliasLock = std::unique_lock{group->operationMutex, std::try_to_lock};
                if (!aliasLock) return false;
            }
            texture->SynchronizeGuest(true, true); // We need to assume the texture is dirty since we don't know what the guest is writing
            return true;
        });
    }

    std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl() {
        if (guest->dimensions != dimensions)
            throw exception("Guest and host dimensions being different is not supported currently");

        if (guest->tileConfig.mode == texture::TileMode::Pitch && mirror.size() < guest->GetSize())
            throw std::out_of_range("Pitch-linear guest mapping is smaller than the texture backing");
        auto pointer{mirror.data()};

        WaitOnBacking();

        u8 *bufferData;
        auto stagingBuffer{[&]() -> std::shared_ptr<memory::StagingBuffer> {
            if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
                // We need a staging buffer for all optimal copies (since we aren't aware of the host optimal layout) and linear textures which we cannot map on the CPU since we do not have access to their backing VkDeviceMemory
                auto stagingBuffer{gpu.memory.AllocateStagingBuffer(surfaceSize)};
                bufferData = stagingBuffer->data();
                return stagingBuffer;
            } else if (tiling == vk::ImageTiling::eLinear) {
                // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly into it rather than a staging buffer
                if (layout == vk::ImageLayout::eUndefined)
                    TransitionLayout(vk::ImageLayout::eGeneral);
                bufferData = std::get<memory::Image>(backing).data();
                WaitOnFence();
                return nullptr;
            } else {
                throw exception("Guest -> Host synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
            }
        }()};

        std::vector<u8> deswizzleBuffer;
        u8 *deswizzleOutput;
        if (guest->format != format) {
            deswizzleBuffer.resize(deswizzledSurfaceSize);
            deswizzleOutput = deswizzleBuffer.data();
        } else [[likely]] {
            deswizzleOutput = bufferData;
        }

        auto guestLayerStride{guest->GetLayerStride()};
        if (levelCount == 1) {
            auto outputLayer{deswizzleOutput};
            for (size_t layer{}; layer < layerCount; layer++) {
                if (guest->tileConfig.mode == texture::TileMode::Block)
                    texture::CopyBlockLinearToLinear(*guest, pointer, outputLayer);
                else if (guest->tileConfig.mode == texture::TileMode::Pitch)
                    texture::CopyPitchLinearToLinear(*guest, {pointer, guestLayerStride}, {outputLayer, deswizzledLayerStride});
                else if (guest->tileConfig.mode == texture::TileMode::Linear)
                    std::memcpy(outputLayer, pointer, surfaceSize);
                pointer += guestLayerStride;
                outputLayer += deswizzledLayerStride;
            }
        } else if (levelCount > 1 && guest->tileConfig.mode == texture::TileMode::Block) {
            // We need to generate a buffer that has all layers for a given mip level while Tegra X1 layout holds all mip levels for a given layer
            for (size_t layer{}; layer < layerCount; layer++) {
                auto inputLevel{pointer}, outputLevel{deswizzleOutput};
                for (const auto &level : mipLayouts) {
                    texture::CopyBlockLinearToLinear(
                        level.dimensions,
                        guest->format->blockWidth, guest->format->blockHeight, guest->format->bpb,
                        level.blockHeight, level.blockDepth,
                        inputLevel, outputLevel + (layer * level.linearSize), level.tileWidthSpacing // Read TIC GOB spacing
                    );

                    inputLevel += level.blockLinearSize; // Skip over the current mip level as we've deswizzled it
                    outputLevel += layerCount * level.linearSize; // We need to offset the output buffer by the size of the previous mip level
                }

                pointer += guestLayerStride; // We need to offset the input buffer by the size of the previous guest layer, this can differ from inputLevel's value due to layer end padding or guest RT layer stride
            }
        } else if (levelCount != 0) {
            throw exception("Mipmapped textures with tiling mode '{}' aren't supported", static_cast<int>(tiling));
        }

        if (!deswizzleBuffer.empty()) {
            for (const auto &level : mipLayouts) {
                auto decode{[&](const u8 *decodeInput, u8 *decodeOutput, size_t decodeHeight) {
                    switch (guest->format->vkFormat) {
                        case vk::Format::eBc1RgbaUnormBlock:
                        case vk::Format::eBc1RgbaSrgbBlock:
                            bcn::DecodeBc1(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, true);
                            break;

                        case vk::Format::eBc2UnormBlock:
                        case vk::Format::eBc2SrgbBlock:
                            bcn::DecodeBc2(decodeInput, decodeOutput, level.dimensions.width, decodeHeight);
                            break;

                        case vk::Format::eBc3UnormBlock:
                        case vk::Format::eBc3SrgbBlock:
                            bcn::DecodeBc3(decodeInput, decodeOutput, level.dimensions.width, decodeHeight);
                            break;

                        case vk::Format::eBc4UnormBlock:
                            bcn::DecodeBc4(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, false);
                            break;
                        case vk::Format::eBc4SnormBlock:
                            bcn::DecodeBc4(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, true);
                            break;

                        case vk::Format::eBc5UnormBlock:
                            bcn::DecodeBc5(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, false);
                            break;
                        case vk::Format::eBc5SnormBlock:
                            bcn::DecodeBc5(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, true);
                            break;

                        case vk::Format::eBc6HUfloatBlock:
                            bcn::DecodeBc6(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, false);
                            break;
                        case vk::Format::eBc6HSfloatBlock:
                            bcn::DecodeBc6(decodeInput, decodeOutput, level.dimensions.width, decodeHeight, true);
                            break;

                        case vk::Format::eBc7UnormBlock:
                        case vk::Format::eBc7SrgbBlock:
                            bcn::DecodeBc7(decodeInput, decodeOutput, level.dimensions.width, decodeHeight);
                            break;

                        default:
                            throw exception("Unsupported guest format '{}'", vk::to_string(guest->format->vkFormat));
                    }
                }};
                if (guest->tileConfig.mode == texture::TileMode::Pitch) {
                    // Joining odd-height BC layers into one tall image discards
                    // each layer's partial block row and shifts subsequent layers.
                    texture::DecodePitchLinearLayers(
                        {deswizzleOutput, deswizzledSurfaceSize}, level.linearSize,
                        {bufferData, surfaceSize}, level.targetLinearSize, layerCount,
                        [&](const u8 *input, u8 *output) { decode(input, output, level.dimensions.height); });
                } else {
                    decode(deswizzleOutput, bufferData, level.dimensions.height * layerCount);
                }

                deswizzleOutput += level.linearSize * layerCount;
                bufferData += level.targetLinearSize * layerCount;
            }
        }

        return stagingBuffer;
    }

    boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies() {
        boost::container::small_vector<vk::BufferImageCopy, 10> bufferImageCopies;

        auto pushBufferImageCopyWithAspect{[&](vk::ImageAspectFlagBits aspect) {
            vk::DeviceSize bufferOffset{};
            u32 mipLevel{};
            for (auto &level : mipLayouts) {
                bufferImageCopies.emplace_back(
                    vk::BufferImageCopy{
                        .bufferOffset = bufferOffset,
                        .imageSubresource = {
                            .aspectMask = aspect,
                            .mipLevel = mipLevel++,
                            .layerCount = layerCount,
                        },
                        .imageExtent = level.dimensions,
                    }
                );
                bufferOffset += level.targetLinearSize * layerCount;
            }
        }};

        if (format->vkAspect & vk::ImageAspectFlagBits::eColor)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eColor);
        if (format->vkAspect & vk::ImageAspectFlagBits::eDepth)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eDepth);
        if (format->vkAspect & vk::ImageAspectFlagBits::eStencil)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eStencil);

        return bufferImageCopies;
    }

    void Texture::CopyFromStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        const auto image{GetBacking()};
        const vk::ImageSubresourceRange range{
            .aspectMask = format->vkAspect,
            .levelCount = levelCount,
            .layerCount = layerCount,
        };

        // Re-uploading an initialized image must depend on prior reads/writes to
        // the same subresources. The old path emitted a barrier only for the
        // Undefined -> General transition, which leaves a WRITE_AFTER_WRITE
        // hazard when an alias/backing is refreshed while already in General.
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
            .image = image,
            .srcAccessMask = layout == vk::ImageLayout::eUndefined ? vk::AccessFlags{} :
                vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = std::exchange(layout, vk::ImageLayout::eGeneral),
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = range,
        });

        auto bufferImageCopies{GetBufferImageCopies()};
        commandBuffer.copyBufferToImage(stagingBuffer->vkBuffer, image, layout,
            vk::ArrayProxy(static_cast<u32>(bufferImageCopies.size()), bufferImageCopies.data()));

        // Make the transfer write visible to the next use without a global
        // execution wait. This dependency is scoped to this image/subresource set.
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
            .image = image,
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = layout,
            .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = range,
        });
    }

    void Texture::CopyIntoStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        const auto image{GetBacking()};
        // BottomOfPipe cannot make earlier shader/attachment/transfer writes
        // available to this copy. Depend on all possible prior writers, but keep
        // the memory dependency scoped to this image.
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
            .image = image,
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = layout,
            .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = {
                .aspectMask = format->vkAspect,
                .levelCount = levelCount,
                .layerCount = layerCount,
            },
        });

        auto bufferImageCopies{GetBufferImageCopies()};
        commandBuffer.copyImageToBuffer(image, layout, stagingBuffer->vkBuffer,
            vk::ArrayProxy(static_cast<u32>(bufferImageCopies.size()), bufferImageCopies.data()));

        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {}, vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = stagingBuffer->vkBuffer,
            .offset = 0,
            .size = stagingBuffer->size(),
        }, {});
    }

    void Texture::CopyToGuest(u8 *hostBuffer) {
        if (guest->tileConfig.mode == texture::TileMode::Pitch && mirror.size() < guest->GetSize())
            throw std::out_of_range("Pitch-linear guest mapping is smaller than the texture backing");
        auto guestOutput{mirror.data()};

        auto guestLayerStride{guest->GetLayerStride()};
        if (levelCount == 1) {
            for (size_t layer{}; layer < layerCount; layer++) {
                if (guest->tileConfig.mode == texture::TileMode::Block)
                    texture::CopyLinearToBlockLinear(*guest, hostBuffer, guestOutput);
                else if (guest->tileConfig.mode == texture::TileMode::Pitch)
                    texture::CopyLinearToPitchLinear(*guest, {hostBuffer, layerStride}, {guestOutput, guestLayerStride});
                else if (guest->tileConfig.mode == texture::TileMode::Linear)
                    std::memcpy(hostBuffer, guestOutput, layerStride);
                guestOutput += guestLayerStride;
                hostBuffer += layerStride;
            }
        } else if (levelCount > 1 && guest->tileConfig.mode == texture::TileMode::Block) {
            // We need to copy into the Tegra X1 layout holds all mip levels for a given layer while the input buffer has all layers for a given mip level
            // Note: See SynchronizeHostImpl for additional comments
            for (size_t layer{}; layer < layerCount; layer++) {
                auto outputLevel{guestOutput}, inputLevel{hostBuffer};
                for (const auto &level : mipLayouts) {
                    texture::CopyLinearToBlockLinear(
                        level.dimensions,
                        guest->format->blockWidth, guest->format->blockHeight, guest->format->bpb,
                        level.blockHeight, level.blockDepth,
                        inputLevel + (layer * level.linearSize), outputLevel, level.tileWidthSpacing
                    );

                    outputLevel += level.blockLinearSize;
                    inputLevel += layerCount * level.linearSize;
                }

                guestOutput += guestLayerStride;
            }
        } else if (levelCount != 0) {
            throw exception("Mipmapped textures with tiling mode '{}' aren't supported", static_cast<int>(tiling));
        }
    }

    void Texture::FreeGuest() {
        // A read-only alias may need the guest representation again after another
        // alias becomes the writer. Preserve those bytes without changing the
        // global Free Guest Texture Memory policy for ordinary textures.
        if (aliasReadOnly.load())
            return;
        // Avoid freeing memory if the backing format doesn't match, as otherwise texture data would be lost on the guest side, also avoid if fast readback is active
        if (!guestMemoryPins && *gpu.state.settings->freeGuestTextureMemory && guest->format == format && !(accumulatedGuestWaitTime > SkipReadbackHackWaitTimeThreshold && *gpu.state.settings->enableFastGpuReadbackHack)) {
            gpu.state.process->memory.FreeMemory(mirror);
            memoryFreed = true;
        }
    }

    Texture::Texture(GPU &gpu, BackingType &&backing, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, vk::ImageCreateFlags flags, vk::ImageUsageFlags usage, u32 levelCount, u32 layerCount, vk::SampleCountFlagBits sampleCount, vk::ImageType imageType)
        : gpu(gpu),
          backing(std::move(backing)),
          backingImageType(imageType),
          dimensions(dimensions),
          format(format),
          layout(layout),
          tiling(tiling),
          flags(flags),
          usage(usage),
          levelCount(levelCount),
          layerCount(layerCount),
          sampleCount(sampleCount) {}

    texture::Format ConvertHostCompatibleFormat(texture::Format format, const TraitManager &traits) {
        auto bcnSupport{traits.bcnSupport};
        if (bcnSupport.all())
            return format;

        switch (format->vkFormat) {
            case vk::Format::eBc1RgbaUnormBlock:
                return bcnSupport[0] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc1RgbaSrgbBlock:
                return bcnSupport[0] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc2UnormBlock:
                return bcnSupport[1] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc2SrgbBlock:
                return bcnSupport[1] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc3UnormBlock:
                return bcnSupport[2] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc3SrgbBlock:
                return bcnSupport[2] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc4UnormBlock:
                return bcnSupport[3] ? format : format::R8Unorm;
            case vk::Format::eBc4SnormBlock:
                return bcnSupport[3] ? format : format::R8Snorm;

            case vk::Format::eBc5UnormBlock:
                return bcnSupport[4] ? format : format::R8G8Unorm;
            case vk::Format::eBc5SnormBlock:
                return bcnSupport[4] ? format : format::R8G8Snorm;

            case vk::Format::eBc6HUfloatBlock:
            case vk::Format::eBc6HSfloatBlock:
                return bcnSupport[5] ? format : format::R16G16B16A16Float; // This is a signed 16-bit FP format, we don't have an unsigned 16-bit FP format

            case vk::Format::eBc7UnormBlock:
                return bcnSupport[6] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc7SrgbBlock:
                return bcnSupport[6] ? format : format::R8G8B8A8Srgb;

            default:
                return format;
        }
    }

    size_t CalculateLevelStride(const std::vector<texture::MipLevelLayout> &mipLayouts) {
        size_t surfaceSize{};
        for (const auto &level : mipLayouts)
            surfaceSize += level.linearSize;
        return surfaceSize;
    }

    size_t CalculateTargetLevelStride(const std::vector<texture::MipLevelLayout> &mipLayouts) {
        size_t surfaceSize{};
        for (const auto &level : mipLayouts)
            surfaceSize += level.targetLinearSize;
        return surfaceSize;
    }

    Texture::Texture(GPU &pGpu, GuestTexture pGuest)
        : gpu(pGpu),
          backingImageType(pGuest.GetImageType()),
          guest(std::move(pGuest)),
          dimensions(guest->dimensions),
          format(ConvertHostCompatibleFormat(guest->format, gpu.traits)),
          layout(vk::ImageLayout::eUndefined),
          tiling(vk::ImageTiling::eOptimal), // Force Optimal due to not adhering to host subresource layout during Linear synchronization
          layerCount(guest->layerCount),
          deswizzledLayerStride(guest->format->GetSize(dimensions)),
          layerStride(format == guest->format ? deswizzledLayerStride : format->GetSize(dimensions)),
          levelCount(guest->mipLevelCount),
          mipLayouts(
              texture::GetBlockLinearMipLayout(
                  guest->dimensions,
                  guest->format->blockHeight, guest->format->blockWidth, guest->format->bpb,
                  format->blockHeight, format->blockWidth, format->bpb,
                  guest->tileConfig.blockHeight, guest->tileConfig.blockDepth,
                  guest->mipLevelCount, guest->tileConfig.tileWidthSpacing
              )
          ),
          deswizzledSurfaceSize(guest->tileConfig.mode == texture::TileMode::Pitch ? texture::PitchLinearCheckedMultiply(CalculateLevelStride(mipLayouts), layerCount) : CalculateLevelStride(mipLayouts) * layerCount),
          surfaceSize(format == guest->format ? deswizzledSurfaceSize : (guest->tileConfig.mode == texture::TileMode::Pitch ? texture::PitchLinearCheckedMultiply(CalculateTargetLevelStride(mipLayouts), layerCount) : CalculateTargetLevelStride(mipLayouts) * layerCount)),
          sampleCount(vk::SampleCountFlagBits::e1),
          flags(texture::GuestImageFormatFlags(format->vkFormat, gpu.traits.quirks.vkImageMutableFormatCostly,
              guest->GetImageType() == vk::ImageType::e2D && format->vkAspect == vk::ImageAspectFlagBits::eColor && !format->IsCompressed())),
          usage(vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled) {
        if ((format->vkAspect & vk::ImageAspectFlagBits::eColor) && !format->IsCompressed())
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (format->vkAspect & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil))
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;

        const auto formatFeatures{gpu.vkPhysicalDevice.getFormatProperties(format->vkFormat).optimalTilingFeatures};
        if (formatFeatures & vk::FormatFeatureFlagBits::eStorageImage)
            usage |= vk::ImageUsageFlagBits::eStorage;

        auto imageType{guest->GetImageType()};
        if (imageType == vk::ImageType::e2D && dimensions.width == dimensions.height && layerCount >= 6)
            flags |= vk::ImageCreateFlagBits::eCubeCompatible;
        else if (imageType == vk::ImageType::e3D)
            flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;

        vk::ImageCreateInfo imageCreateInfo{
            .flags = flags,
            .imageType = imageType,
            .format = *format,
            .extent = dimensions,
            .mipLevels = levelCount,
            .arrayLayers = layerCount,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = tiling,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
            .initialLayout = layout,
        };
        backing = tiling != vk::ImageTiling::eLinear ? gpu.memory.AllocateImage(imageCreateInfo) : gpu.memory.AllocateMappedImage(imageCreateInfo);

        // TextureManager sets up weak trap callbacks after shared ownership exists.
    }

    Texture::~Texture() {
        SynchronizeGuest(true);
        if (trapHandle)
            gpu.state.process->trap.DeleteTrap(*trapHandle);
        if (alignedMirror.valid())
            munmap(alignedMirror.data(), alignedMirror.size());
    }

    void Texture::lock() {
        mutex.lock();
        accumulatedCpuLockCounter++;
    }

    bool Texture::LockWithTag(ContextTag pTag) {
        if (pTag && pTag == tag)
            return false;

        mutex.lock();
        tag = pTag;
        return true;
    }

    void Texture::unlock() {
        tag = ContextTag{};
        mutex.unlock();
    }

    bool Texture::try_lock() {
        if (mutex.try_lock()) {
            accumulatedCpuLockCounter++;
            return true;
        }

        return false;
    }

    bool Texture::WaitOnBacking() {
        TRACE_EVENT("gpu", "Texture::WaitOnBacking");

        if (GetBacking()) [[likely]] {
            return false;
        } else {
            std::unique_lock lock(mutex, std::adopt_lock);
            backingCondition.wait(lock, [&]() -> bool { return GetBacking(); });
            lock.release();
            return true;
        }
    }

    void Texture::WaitOnFence() {
        TRACE_EVENT("gpu", "Texture::WaitOnFence");

        if (cycle) {
            cycle->Wait();
            cycle = nullptr;
        }
    }

    void Texture::SwapBacking(BackingType &&pBacking, vk::ImageCreateFlags pFlags, vk::ImageLayout pLayout, std::vector<vk::Format> pViewFormats) {
        WaitOnFence();

        std::vector<vk::ImageView> handles;
        for (const auto &view : views) handles.push_back(*view.vkView);
        auto retiredFramebuffers{gpu.framebufferCache.RetireViews(handles)};
        retiredFramebuffers.clear(); // Fence is complete; destroy framebuffers before their views.
        views.clear();
        ++backingGeneration;

        std::visit([&](auto &replacement) {
            backing.emplace<std::decay_t<decltype(replacement)>>(std::move(replacement));
        }, pBacking);
        flags = pFlags;
        backingViewFormats = std::move(pViewFormats);
        layout = pLayout;
        if (GetBacking())
            backingCondition.notify_all();
    }

    void Texture::TransitionLayout(vk::ImageLayout pLayout) {
        WaitOnBacking();
        // Descriptor preparation repeatedly requests General for storage images.
        // An unchanged layout must retain the asynchronous resource cycle rather
        // than waiting for a presentation read that needs no layout transition.
        if (layout == pLayout)
            return;
        WaitOnFence();

        TRACE_EVENT("gpu", "Texture::TransitionLayout");

        if (layout != pLayout) {
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = GetBacking(),
                    .srcAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .dstAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .oldLayout = std::exchange(layout, pLayout),
                    .newLayout = pLayout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = {
                        .aspectMask = format->vkAspect,
                        .levelCount = levelCount,
                        .layerCount = layerCount,
                    },
                });
            })};
            lCycle->AttachObject(shared_from_this());
            cycle = lCycle;
        }
    }

    bool Texture::CanMaterializeGuestLayout(const GuestTexture &value) {
        if (!value.format || !value.format->bpb || value.format->vkAspect != vk::ImageAspectFlagBits::eColor ||
            value.baseArrayLayer || !value.layerCount || !value.dimensions.width || !value.dimensions.height ||
            !value.dimensions.depth || !value.mipLevelCount)
            return false;

        const auto imageType{value.GetImageType()};
        const auto maxDimension{std::max({value.dimensions.width, value.dimensions.height, value.dimensions.depth})};
        if (value.mipLevelCount > static_cast<u32>(std::bit_width(maxDimension)) ||
            (imageType == vk::ImageType::e3D && value.layerCount != 1))
            return false;

        size_t layerBytes{};
        switch (value.tileConfig.mode) {
            case texture::TileMode::Block: {
                if (!std::has_single_bit(static_cast<u32>(value.tileConfig.blockHeight)) ||
                    !std::has_single_bit(static_cast<u32>(value.tileConfig.blockDepth)))
                    return false;
                auto layouts{texture::GetBlockLinearMipLayout(value.dimensions,
                    value.format->blockHeight, value.format->blockWidth, value.format->bpb,
                    value.format->blockHeight, value.format->blockWidth, value.format->bpb,
                    value.tileConfig.blockHeight, value.tileConfig.blockDepth, value.mipLevelCount, value.tileConfig.tileWidthSpacing)};
                size_t uploadOffset{};
                const auto uploadAlignment{std::lcm<size_t>(4, value.format->bpb)};
                for (const auto &level : layouts) {
                    // This is the packing used by SynchronizeHostImpl/GetBufferImageCopies.
                    if (uploadOffset % uploadAlignment) return false;
                    if (level.linearSize > SIZE_MAX / value.layerCount) return false;
                    const auto packedLevelBytes{level.linearSize * value.layerCount};
                    if (packedLevelBytes > SIZE_MAX - uploadOffset ||
                        level.blockLinearSize > SIZE_MAX - layerBytes)
                        return false;
                    uploadOffset += packedLevelBytes;
                    layerBytes += level.blockLinearSize;
                }
                break;
            }
            case texture::TileMode::Pitch:
            case texture::TileMode::Linear:
                // SynchronizeHostImpl supports these layouts only for a single mip.
                if (value.mipLevelCount != 1 || imageType == vk::ImageType::e3D)
                    return false;
                layerBytes = value.CalculateLayerSize();
                break;
        }

        const size_t stride{value.layerStride ? value.layerStride : value.CalculateLayerSize()};
        size_t mapped{};
        for (const auto &mapping : value.mappings) {
            if (!mapping.valid() || mapping.size() > SIZE_MAX - mapped) return false;
            mapped += mapping.size();
        }
        return stride && layerBytes && layerBytes <= stride && layerBytes <= mapped &&
            value.layerCount - 1 <= (mapped - layerBytes) / stride;
    }

    bool Texture::IsGuestLayoutReversible(const GuestTexture &value) {
        // Writer authority remains deliberately narrower than materialization:
        // the alias group currently has a proven lossless GPU -> guest path only
        // for native 2D block-linear color layouts with non-interleaved Z.
        return CanMaterializeGuestLayout(value) &&
            value.GetImageType() == vk::ImageType::e2D &&
            value.tileConfig.mode == texture::TileMode::Block &&
            value.tileConfig.blockDepth == 1;
    }

    bool Texture::CanReconcileGuestLayout(const GuestTexture &requested) const {
        std::scoped_lock stateLock{stateMutex};
        // A converted format cannot be re-encoded losslessly (e.g. decoded BCn).
        // Discarded padding/tails also cannot be recovered from this image.
        return guest && format == guest->format && ConvertHostCompatibleFormat(requested.format, gpu.traits) == requested.format && !memoryFreed &&
            sampleCount == vk::SampleCountFlagBits::e1 && tiling == vk::ImageTiling::eOptimal &&
            IsGuestLayoutReversible(*guest) && IsGuestLayoutReversible(requested) &&
            guest->mappings.size() == requested.mappings.size() &&
            std::equal(guest->mappings.begin(), guest->mappings.end(), requested.mappings.begin(),
                [](const auto &a, const auto &b) { return a.data() == b.data() && a.size() == b.size(); });
    }

    std::shared_ptr<texture::AliasGroup> Texture::RegisterAliases(std::vector<std::shared_ptr<Texture>> textures, const GuestTexture *requested) {
        if (textures.empty()) return {};
        // The requested layout also covers a cache containing only ineligible
        // history. Direct callers use their first eligible member as the reference.
        const auto reference{textures.front()};
        for (size_t i{}; i < textures.size(); ++i) {
            if (auto group{textures[i]->aliasGroup.load()}) {
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}; member && !member->aliasRetired.load() && std::find(textures.begin(), textures.end(), member) == textures.end())
                        textures.push_back(std::move(member));
            }
        }
        std::sort(textures.begin(), textures.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
        // Registration runs after executor submission releases its resource locks.
        // Trap callbacks only try-lock these resources while holding stateMutex.
        std::vector<std::unique_lock<Texture>> locks;
        for (auto &texture : textures) locks.emplace_back(*texture);
        if (!requested) {
            if (!reference->guest || !reference->CanReconcileGuestLayout(*reference->guest))
                throw exception("Cannot losslessly reconcile these texture alias layouts");
            requested = &*reference->guest;
        }
        if (!IsGuestLayoutReversible(*requested) || ConvertHostCompatibleFormat(requested->format, reference->gpu.traits) != requested->format)
            throw exception("Cannot losslessly reconcile the requested guest layout");
        const auto newest{*std::max_element(textures.begin(), textures.end(), [](const auto &a, const auto &b) {
            return a->gpuWriteSerial < b->gpuWriteSerial;
        })};

        // Free Guest Texture Memory may have discarded the guest pages of the
        // latest native GPU writer before a second representation is requested.
        // Restore those bytes from the writer before classifying aliases: guest
        // memory is the bridge between different Tegra layouts, but it must not
        // be treated as authoritative until this readback has completed.
        bool restoreNewest{};
        {
            std::scoped_lock stateLock{newest->stateMutex};
            restoreNewest = newest->guest && !newest->aliasStale.load() && newest->memoryFreed &&
                newest->dirtyState == DirtyState::GpuDirty && newest->format == newest->guest->format &&
                newest->sampleCount == vk::SampleCountFlagBits::e1 &&
                IsGuestLayoutReversible(*newest->guest);
        }
        if (restoreNewest) {
            DirtyState previousState;
            bool previousMemoryFreed{};
            {
                std::scoped_lock stateLock{newest->stateMutex};
                previousState = newest->dirtyState;
                previousMemoryFreed = newest->memoryFreed;
            }
            try {
                newest->SynchronizeGuest(false, true);
                std::scoped_lock stateLock{newest->stateMutex};
                if (newest->memoryFreed || newest->gpuWriteSerial > newest->gpuReadbackSerial)
                    throw exception("Texture alias writer guest bridge was not restored");
            } catch (...) {
                std::scoped_lock stateLock{newest->stateMutex};
                newest->dirtyState = previousState;
                newest->memoryFreed = previousMemoryFreed;
                throw;
            }
        }

        std::vector<std::shared_ptr<Texture>> eligible, historical;
        for (const auto &texture : textures) {
            if (!texture->aliasRetired.load() && texture->CanReconcileGuestLayout(*requested))
                eligible.push_back(texture);
            else
                historical.push_back(texture);
        }
        // A non-reversible GPU writer cannot be encoded through guest memory.
        // Retirement is safe only after readback or a strictly newer writer has
        // superseded it. An untracked/tied writer is deliberately rejected.
        for (const auto &texture : historical) {
            std::scoped_lock stateLock{texture->stateMutex};
            bool unpreserved{texture->dirtyState == DirtyState::GpuDirty || texture->memoryFreed ||
                texture->gpuWriteSerial > texture->gpuReadbackSerial};
            if (auto previousGroup{texture->aliasGroup.load()}) {
                std::scoped_lock metadata{previousGroup->mutex};
                unpreserved |= previousGroup->writerIdentity == texture.get() &&
                    previousGroup->guestGeneration != previousGroup->generation;
            }
            if (!texture->aliasStale.load() && unpreserved &&
                texture->gpuWriteSerial >= newest->gpuWriteSerial)
                throw exception("Cannot retire an authoritative non-reversible texture alias");
        }
        auto group{std::make_shared<texture::AliasGroup>()};
        group->members.reserve(eligible.size());
        for (const auto &texture : eligible) group->members.emplace_back(texture);
        // A surviving stale member is not the current image, even if it has
        // the highest remaining serial after historical writer retirement.
        if (newest->aliasStale.load()) {
            const auto previousGroup{newest->aliasGroup.load()};
            if (!previousGroup)
                throw exception("Stale texture alias has no authoritative guest generation");
            std::scoped_lock operation{previousGroup->operationMutex};
            std::scoped_lock metadata{previousGroup->mutex};
            if (previousGroup->guestGeneration != previousGroup->generation)
                throw exception("Stale texture alias cannot replace an unpreserved writer");
        }
        const bool newestEligible{!newest->aliasStale.load() && std::find(eligible.begin(), eligible.end(), newest) != eligible.end()};
        if (!eligible.empty()) {
            const auto writer{newestEligible ? newest : eligible.front()};
            group->writer = writer;
            group->writerIdentity = writer.get();
        }
        // Only the newest writer may encode these bytes. A completed readback
        // from even a historical layout already makes guest memory authoritative.
        if (newestEligible) {
            DirtyState previousState;
            bool previousMemoryFreed{};
            {
                std::scoped_lock stateLock{newest->stateMutex};
                previousState = newest->dirtyState;
                previousMemoryFreed = newest->memoryFreed;
            }
            try {
                newest->SynchronizeGuest(false, true);
                std::scoped_lock stateLock{newest->stateMutex};
                if (previousState != DirtyState::CpuDirty && newest->gpuWriteSerial > newest->gpuReadbackSerial)
                    throw exception("Texture alias writer has no completed guest encoding");
            } catch (...) {
                std::scoped_lock stateLock{newest->stateMutex};
                newest->dirtyState = previousState;
                newest->memoryFreed = previousMemoryFreed;
                throw;
            }
        }
        // No retirement/publication before the authoritative readback succeeds.
        for (const auto &texture : historical) {
            texture->aliasStale = true;
            texture->aliasRetired = true;
            texture->replaced = true; // Exclude manager lookups; retained views are guarded too.
        }
        for (auto &texture : eligible) {
            texture->aliasStale = !newestEligible || texture != newest;
            texture->aliasGroup.store(group);
        }
        return group;
    }

    void Texture::RequireAliasCoherence(ContextTag context) {
        if (aliasRetired.load())
            throw exception("Retired texture alias must be resolved through the texture manager");
        if (!aliasStale.load()) return;
        // Caller may already own this Texture through the executor. Never take
        // another Texture's lock here; unwind and release all executor locks first.
        (void)context;
        throw TextureAliasReconciliationRequest{shared_from_this(), {}};
    }

    void Texture::ReconcileAliases() {
        if (aliasRetired.load())
            throw exception("Cannot reconcile a retired non-reversible texture alias");
        auto group{aliasGroup.load()};
        if (!group || !aliasStale.load()) return;
        std::shared_ptr<Texture> writer;
        u64 generation{};
        bool download{};
        {
            std::scoped_lock operation{group->operationMutex}; // No resource locks held; let an expiring writer finish its readback.
            std::scoped_lock metadata{group->mutex};
            generation = group->generation;
            writer = group->writer.lock();
            download = group->guestGeneration != generation;
        }
        if (download) {
            if (!writer) throw exception("Texture alias writer disappeared before reconciliation");
            std::scoped_lock writerLock{*writer};
            writer->SynchronizeGuest(false, true); // Wait only on this writer/copy, then re-encode its layout.
        }
        // Do not hold the source lock while acquiring the target lock. Metadata
        // is checked again to avoid publishing an obsolete generation.
        std::scoped_lock targetLock{*this};
        std::scoped_lock stateLock{stateMutex};
        std::scoped_lock metadata{group->mutex};
        if (group->generation != generation || group->guestGeneration != generation)
            throw exception("Texture alias generation changed during reconciliation");
        dirtyState = DirtyState::CpuDirty;
        memoryFreed = false;
        aliasStale = false;
        // SynchronizeHost uploads lazily on attachment. Failure leaves CpuDirty,
        // so the upload can be retried without losing the authoritative writer.
    }

    void Texture::ValidateAliasWrites(span<TextureView *> writers) {
        for (size_t i{}; i < writers.size(); ++i) {
            if (!writers[i]) continue;
            auto owner{writers[i]->texture.get()};
            if (owner->aliasReadOnly.load())
                throw exception("A materialized read-only texture alias cannot be used as a render target");
            auto group{owner->aliasGroup.load()};
            if (!group) continue;
            for (size_t j{}; j < i; ++j)
                if (writers[j] && writers[j]->texture.get() != owner && writers[j]->texture->aliasGroup.load() == group)
                    throw exception("Concurrent writes through distinct texture alias layouts cannot be losslessly ordered");
        }
    }

    std::shared_ptr<void> Texture::PinGuestMemory() {
        std::scoped_lock stateLock{stateMutex};
        if (!guest || memoryFreed)
            throw exception("Guest bytes needed for texture alias reconciliation are unavailable");
        const auto self{shared_from_this()};
        ++guestMemoryPins;
        // shared_ptr invokes the deleter even if allocating its control block
        // fails, balancing the pin on both success and failure paths.
        return std::shared_ptr<void>{this, [texture = self](void *) {
            std::scoped_lock lock{texture->stateMutex};
            --texture->guestMemoryPins;
        }};
    }

    void Texture::RecordGpuWrite() {
        if (aliasReadOnly.load())
            throw exception("Cannot write through a materialized read-only texture alias");
        if (aliasRetired.load())
            throw exception("Cannot write through a retired non-reversible texture alias");
        auto alias{aliasGroup.load()};
        std::unique_lock<std::recursive_mutex> aliasLock;
        if (alias) aliasLock = std::unique_lock{alias->operationMutex};
        if (!guest) return;
        everUsedAsGpuWrite = true;
        gpuWriteSerial = ++texture::gpuWriteSequence;
        if (auto group{aliasGroup.load()}) {
            {
                std::scoped_lock metadata{group->mutex};
                ++group->generation;
                group->writer = shared_from_this();
                group->writerIdentity = this;
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}) member->aliasStale = member.get() != this;
            }
            std::scoped_lock stateLock{stateMutex};
            // Grouped images are uploaded on attachment before their first write.
            dirtyState = DirtyState::GpuDirty;
            gpu.state.process->trap.TrapRegions(*trapHandle, false);
        }
    }

    void Texture::MarkGpuDirty(UsageTracker &usageTracker) {
        RecordGpuWrite();
        for (auto mapping : guest->mappings)
            if (mapping.valid())
                usageTracker.dirtyIntervals.Insert(mapping);
    }

    void Texture::SynchronizeHost(bool gpuDirty) {
        if (gpuDirty && aliasReadOnly.load())
            throw exception("Cannot mark a materialized read-only texture alias GPU-dirty");
        auto alias{aliasGroup.load()};
        std::unique_lock<std::recursive_mutex> aliasLock;
        if (alias) aliasLock = std::unique_lock{alias->operationMutex};
        if (!guest)
            return;

        // With Free Guest Texture Memory disabled, generic executor attachment
        // synchronization must not invent GPU ownership for sampled-only images.
        // RT and storage paths call RecordGpuWrite before submission.
        if (!*gpu.state.settings->freeGuestTextureMemory && !everUsedAsGpuWrite)
            gpuDirty = false;

        TRACE_EVENT("gpu", "Texture::SynchronizeHost");
        {
            std::scoped_lock lock{stateMutex};
            if (gpuDirty && dirtyState == DirtyState::Clean) {
                // If a texture is Clean then we can just transition it to being GPU dirty and retrap it
                dirtyState = DirtyState::GpuDirty;
                gpu.state.process->trap.TrapRegions(*trapHandle, false);
                FreeGuest();
                return;
            } else if (dirtyState != DirtyState::CpuDirty) {
                return; // If the texture has not been modified on the CPU, there is no need to synchronize it
            }

            dirtyState = gpuDirty ? DirtyState::GpuDirty : DirtyState::Clean;
            gpu.state.process->trap.TrapRegions(*trapHandle, !gpuDirty); // Trap any future CPU reads (optionally) + writes to this texture
        }

        // From this point on Clean -> CPU dirty state transitions can occur, GPU dirty -> * transitions will always require the full lock to be held and thus won't occur

        const auto previousLayout{layout};
        try {
            auto stagingBuffer{SynchronizeHostImpl()};
            if (stagingBuffer) {
                if (cycle) cycle->WaitSubmit();
                auto commandBuffer{gpu.scheduler.AllocateCommandBuffer()};
                auto uploadCycle{commandBuffer.GetFenceCycle()};
                try {
                    // Prepare lifetimes before submission for every texture, not
                    // just aliases. A failed attachment must not strand an upload.
                    uploadCycle->AttachObjects(stagingBuffer, shared_from_this());
                    uploadCycle->ChainCycle(cycle);
                    commandBuffer->begin(vk::CommandBufferBeginInfo{
                        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
                    });
                    CopyFromStagingBuffer(*commandBuffer, stagingBuffer);
                    commandBuffer->end();
                    gpu.scheduler.SubmitCommandBuffer(*commandBuffer, uploadCycle);
                } catch (...) {
                    uploadCycle->Cancel();
                    throw;
                }
                cycle = std::move(uploadCycle);
            }
        } catch (...) {
            // Neither a failed upload nor a recorded-but-unsubmitted layout
            // transition transfers authority away from the CPU. Retrying must
            // upload again before FreeGuest can discard the source bytes.
            layout = previousLayout;
            std::scoped_lock lock{stateMutex};
            dirtyState = DirtyState::CpuDirty;
            throw;
        }

        {
            std::scoped_lock lock{stateMutex};

            if (dirtyState == DirtyState::GpuDirty)
                FreeGuest();
        }
    }

    void Texture::SynchronizeHostInline(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &pCycle, bool gpuDirty) {
        if (!guest)
            return;

        // SubmitInternal passes gpuDirty=true for every attached texture,
        // including sampled/read-only resources. Alias writer ownership is
        // established by RecordGpuWrite at the actual RT/storage write sites;
        // attachment synchronization itself is not evidence of a GPU write.
        if (HasAliases()) gpuDirty = false;
        if (gpuDirty && aliasReadOnly.load())
            throw exception("Cannot mark a materialized read-only texture alias GPU-dirty");

        TRACE_EVENT("gpu", "Texture::SynchronizeHostInline");
        // Preserve the Free Guest Texture Memory policy while still tracking
        // real storage/RT writers when the option is disabled.
        if (!*gpu.state.settings->freeGuestTextureMemory && !everUsedAsGpuWrite)
            gpuDirty = false;

        {
            std::scoped_lock lock{stateMutex};
            if (gpuDirty && dirtyState == DirtyState::Clean) {
                dirtyState = DirtyState::GpuDirty;
                gpu.state.process->trap.TrapRegions(*trapHandle, false);
                FreeGuest();
                return;
            } else if (dirtyState != DirtyState::CpuDirty) {
                return;
            }

            dirtyState = gpuDirty ? DirtyState::GpuDirty : DirtyState::Clean;
            gpu.state.process->trap.TrapRegions(*trapHandle, !gpuDirty); // Trap any future CPU reads (optionally) + writes to this texture
        }

        const auto previousLayout{layout};
        try {
            auto stagingBuffer{SynchronizeHostImpl()};
            if (stagingBuffer) {
                // These allocations can fail. Keep the source CPU-authoritative
                // and the old cycle until the upload has been fully recorded.
                pCycle->AttachObjects(stagingBuffer, shared_from_this());
                pCycle->ChainCycle(cycle);
                CopyFromStagingBuffer(commandBuffer, stagingBuffer);
                cycle = pCycle;
            }
        } catch (...) {
            layout = previousLayout;
            std::scoped_lock lock{stateMutex};
            dirtyState = DirtyState::CpuDirty;
            throw;
        }

        {
            std::scoped_lock lock{stateMutex};

            if (dirtyState == DirtyState::GpuDirty)
                FreeGuest();
        }
    }

    void Texture::SynchronizeGuest(bool cpuDirty, bool skipTrap) {
        // Read-only aliases are reconstructed from authoritative guest bytes.
        // They must never become a GPU -> guest encoding source.
        if (aliasReadOnly.load())
            return;
        auto alias{aliasGroup.load()};
        std::unique_lock<std::recursive_mutex> aliasLock;
        if (alias) aliasLock = std::unique_lock{alias->operationMutex};
        if (aliasStale.load()) return; // A superseded GPU image must never overwrite newer guest bytes.
        if (!guest)
            return;

        TRACE_EVENT("gpu", "Texture::SynchronizeGuest");

        {
            std::scoped_lock lock{stateMutex};
            if (cpuDirty && dirtyState == DirtyState::Clean) {
                dirtyState = DirtyState::CpuDirty;
                if (!skipTrap)
                    gpu.state.process->trap.DeleteTrap(*trapHandle);
                return;
            } else if (dirtyState != DirtyState::GpuDirty) {
                return;
            }

            if (!alias) {
                dirtyState = cpuDirty ? DirtyState::CpuDirty : DirtyState::Clean;
                memoryFreed = false;
            } // Group writer remains authoritative until the encoded readback succeeds.
        }

        if (layout == vk::ImageLayout::eUndefined || format != guest->format)
            // If the state of the host texture is undefined then so can the guest
            // If the texture has differing formats on the guest and host, we don't support converting back in that case as it may involve recompression of a decompressed texture
            return;

        WaitOnBacking();

        if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
            if (!downloadStagingBuffer)
                downloadStagingBuffer = gpu.memory.AllocateStagingBuffer(surfaceSize);

            WaitOnFence();
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                CopyIntoStagingBuffer(commandBuffer, downloadStagingBuffer);
            })};
            lCycle->Wait(); // We block till the copy is complete

            CopyToGuest(downloadStagingBuffer->data());
        } else if (tiling == vk::ImageTiling::eLinear) {
            // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly from it rather than using a staging buffer
            WaitOnFence();
            CopyToGuest(std::get<memory::Image>(backing).data());
        } else {
            throw exception("Host -> Guest synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
        }

        {
            std::scoped_lock lock{stateMutex};
            gpuReadbackSerial = gpuWriteSerial;
            if (alias) {
                dirtyState = cpuDirty ? DirtyState::CpuDirty : DirtyState::Clean;
                memoryFreed = false;
            }
        }
        if (auto group{aliasGroup.load()}) {
            std::scoped_lock metadata{group->mutex};
            if (group->writerIdentity == this)
                group->guestGeneration = group->generation;
        }
        if (!skipTrap) {
            if (cpuDirty)
                gpu.state.process->trap.DeleteTrap(*trapHandle);
            else
                gpu.state.process->trap.TrapRegions(*trapHandle, true); // Trap any future CPU writes to this texture
        }
    }

    bool Texture::ViewFormatAdmitted(vk::Format requested) const {
        return texture::ViewFormatAdmitted(format->vkFormat, flags, backingViewFormats, requested);
    }

    bool Texture::ViewRangeFits(vk::ImageViewType type, const vk::ImageSubresourceRange &range, u32 levels) const {
        return texture::ViewRangeFits(backingImageType, dimensions, flags, levels ? levels : levelCount, layerCount, type, range);
    }

    bool Texture::CanGrowMipLevels(u32 levels) const {
        std::scoped_lock stateLock{stateMutex};
        if (!guest || mipLayouts.size() != levelCount || levels <= levelCount || levels > static_cast<u32>(std::bit_width(std::max(dimensions.width, dimensions.height))) ||
            backingImageType != vk::ImageType::e2D || guest->GetImageType() != vk::ImageType::e2D || guest->baseArrayLayer != 0 ||
            guest->tileConfig.mode != texture::TileMode::Block || guest->tileConfig.blockDepth != 1 ||
            format != guest->format || format->vkAspect != vk::ImageAspectFlagBits::eColor ||
            sampleCount != vk::SampleCountFlagBits::e1 || tiling != vk::ImageTiling::eOptimal || memoryFreed)
            return false;
        auto layouts{texture::GetBlockLinearMipLayout(dimensions,
            format->blockHeight, format->blockWidth, format->bpb,
            format->blockHeight, format->blockWidth, format->bpb,
            guest->tileConfig.blockHeight, guest->tileConfig.blockDepth, levels, guest->tileConfig.tileWidthSpacing)};
        size_t guestSize{};
        for (size_t level{}; level < layouts.size(); ++level) {
            const auto &next{layouts[level]};
            if (level < mipLayouts.size()) {
                const auto &old{mipLayouts[level]};
                if (old.dimensions != next.dimensions || old.blockLinearSize != next.blockLinearSize ||
                    old.linearSize != next.linearSize || old.targetLinearSize != next.targetLinearSize ||
                    old.blockHeight != next.blockHeight || old.blockDepth != next.blockDepth)
                    return false;
            }
            if (next.blockLinearSize > guest->layerStride || guestSize > guest->layerStride - next.blockLinearSize)
                return false;
            guestSize += next.blockLinearSize;
        }
        // The mapping can include layer-end padding, but every new mip must fit
        // within the original stride and the final layer's mapped bytes.
        return layerCount && guestSize <= mirror.size() &&
               layerCount - 1 <= (mirror.size() - guestSize) / guest->layerStride;
    }

    std::shared_ptr<void> Texture::PinGuestMemoryForMipGrowth(u32 levels) {
        std::scoped_lock stateLock{stateMutex};
        if (!CanGrowMipLevels(levels))
            throw exception("Guest bytes needed for mip expansion are unavailable or have an incompatible layout");
        return PinGuestMemory();
    }

    void Texture::PromoteBackingForView(vk::Format requested, u32 requestedLevels) {
        const auto nextLevels{std::max(levelCount, requestedLevels)};
        const bool grow{nextLevels != levelCount};
        const bool admitFormat{!ViewFormatAdmitted(requested)};
        if (!grow && !admitFormat) return;
        std::scoped_lock stateLock{stateMutex};
        if (grow && !CanGrowMipLevels(nextLevels))
            throw exception("Cannot preserve this texture's guest mip layout while growing its backing");
        if (!guest || guest->GetImageType() != vk::ImageType::e2D ||
            format->vkAspect != vk::ImageAspectFlagBits::eColor || (admitFormat && format->IsCompressed()) ||
            sampleCount != vk::SampleCountFlagBits::e1 || tiling != vk::ImageTiling::eOptimal ||
            !(usage & vk::ImageUsageFlagBits::eTransferSrc) || !(usage & vk::ImageUsageFlagBits::eTransferDst))
            throw exception("Cannot safely promote this texture backing to format {}", vk::to_string(requested));

        WaitOnBacking();
        // When CPU bytes own the existing levels, upload them before copying the
        // old image. GPU-dirty levels must never be overwritten from the mirror.
        if (grow) SynchronizeHost(false);
        if (grow && layout == vk::ImageLayout::eUndefined)
            throw exception("Cannot grow a backing without initialized source mip levels");
        const auto previousCycle{cycle};
        if (previousCycle) previousCycle->WaitSubmit(); // Old raw views/nodes must finish recording first.
        auto admitted{admitFormat ? texture::AdmitViewFormat(format->vkFormat, backingViewFormats, requested) : backingViewFormats};
        const auto promotedFlags{admitFormat ? flags | vk::ImageCreateFlagBits::eMutableFormat : flags};
        vk::ImageFormatListCreateInfo list{
            .viewFormatCount = static_cast<u32>(admitted.size()), .pViewFormats = admitted.data()
        };
        vk::ImageCreateInfo createInfo{
            .pNext = gpu.traits.supportsImageFormatList && !admitted.empty() ? &list : nullptr,
            .flags = promotedFlags, .imageType = guest->GetImageType(), .format = *format,
            .extent = dimensions, .mipLevels = nextLevels, .arrayLayers = layerCount,
            .samples = sampleCount, .tiling = tiling, .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive, .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex, .initialLayout = vk::ImageLayout::eUndefined,
        };
        auto next{std::make_shared<memory::Image>(gpu.memory.AllocateImage(createInfo))};
        const auto oldImage{GetBacking()};
        const auto newImage{next->vkImage};
        auto copies{texture::PromotionCopyRegions(dimensions, levelCount, layerCount)};
        std::vector<texture::MipLevelLayout> nextLayouts;
        std::vector<vk::BufferImageCopy> uploads;
        std::shared_ptr<memory::StagingBuffer> initialization;
        size_t nextSurfaceSize{surfaceSize};
        if (grow) {
            nextLayouts = texture::GetBlockLinearMipLayout(dimensions,
                format->blockHeight, format->blockWidth, format->bpb,
                format->blockHeight, format->blockWidth, format->bpb,
                guest->tileConfig.blockHeight, guest->tileConfig.blockDepth, nextLevels, guest->tileConfig.tileWidthSpacing);
            nextSurfaceSize = CalculateTargetLevelStride(nextLayouts) * layerCount;
            size_t stagingSize{};
            const auto alignment{std::lcm<size_t>(4, format->bpb)};
            for (u32 level{levelCount}; level < nextLevels; ++level) {
                stagingSize += (alignment - stagingSize % alignment) % alignment;
                uploads.push_back(vk::BufferImageCopy{
                    .bufferOffset = stagingSize,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, layerCount},
                    .imageExtent = nextLayouts[level].dimensions,
                });
                stagingSize += nextLayouts[level].targetLinearSize * layerCount;
            }
            initialization = gpu.memory.AllocateStagingBuffer(stagingSize);
            size_t guestOffset{};
            for (u32 level{}; level < nextLevels; ++level) {
                const auto &mip{nextLayouts[level]};
                if (level >= levelCount)
                    for (u32 layer{}; layer < layerCount; ++layer)
                        texture::CopyBlockLinearToLinear(mip.dimensions,
                            format->blockWidth, format->blockHeight, format->bpb, mip.blockHeight, mip.blockDepth,
                            mirror.data() + layer * static_cast<size_t>(guest->layerStride) + guestOffset,
                            initialization->data() + uploads[level - levelCount].bufferOffset + layer * mip.targetLinearSize,
                            mip.tileWidthSpacing);
                guestOffset += mip.blockLinearSize;
            }
        }
        struct Retired {
            BackingType backing;
            std::vector<TextureViewStorage> views;
            std::vector<vk::raii::Framebuffer> framebuffers; // Destroy before views and image.
        };
        auto retired{std::make_shared<Retired>()};
        std::vector<vk::ImageView> handles;
        for (const auto &view : views) handles.push_back(*view.vkView);
        // A failed promotion may still retire cache entries used by old GPU work.
        // Attach before cache mutation, even when the copy has not been submitted.
        if (previousCycle) previousCycle->AttachObject(retired);
        retired->framebuffers = gpu.framebufferCache.RetireViews(handles);
        std::shared_ptr<FenceCycle> copyCycle{previousCycle};
        if (layout != vk::ImageLayout::eUndefined) {
            auto commandBuffer{gpu.scheduler.AllocateCommandBuffer()};
            copyCycle = commandBuffer.GetFenceCycle();
            try {
                // Prepare all allocating lifetime/dependency operations before submission.
                copyCycle->AttachObjects(retired, next, shared_from_this());
                if (initialization) copyCycle->AttachObject(initialization);
                copyCycle->ChainCycle(previousCycle);
                commandBuffer->begin(vk::CommandBufferBeginInfo{
                    .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
                });
                texture::RecordBackingPromotion(*commandBuffer, oldImage, newImage, layout, levelCount, layerCount, copies,
                    nextLevels, initialization ? initialization->vkBuffer : vk::Buffer{}, uploads);
                commandBuffer->end();
                gpu.scheduler.SubmitCommandBuffer(*commandBuffer, copyCycle);
            } catch (...) {
                copyCycle->Cancel();
                throw; // No backing ownership or view generation has changed.
            }
        }

        // No allocating operations after submission. Image's move constructor transfers
        // ownership; its defaulted move assignment would duplicate the VMA allocation.
        std::visit([&](auto &old) {
            retired->backing.emplace<std::decay_t<decltype(old)>>(std::move(old));
        }, backing);
        retired->views = std::move(views);
        backing.emplace<memory::Image>(std::move(*next));
        flags = promotedFlags;
        backingViewFormats = gpu.traits.supportsImageFormatList ? std::move(admitted) : std::vector<vk::Format>{};
        if (grow) {
            // Preserve mappings/trap identity: TextureManager stores iterators into
            // this mapping vector, and every cached alias shares this Texture.
            guest->mipLevelCount = nextLevels;
            levelCount = nextLevels;
            mipLayouts = std::move(nextLayouts);
            surfaceSize = deswizzledSurfaceSize = nextSurfaceSize; // Native-format growth only.
            downloadStagingBuffer.reset();
        }
        ++backingGeneration;
        cycle = std::move(copyCycle);
    }

    std::shared_ptr<TextureView> Texture::GetView(vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format pFormat, vk::ComponentMapping mapping, ContextTag context) {
        RequireAliasCoherence(context);
        if (!ViewRangeFits(type, range))
            throw exception("Texture view exceeds backing mip/layer bounds or has an incompatible view type");
        if (!pFormat || (guest && pFormat == guest->format))
            pFormat = format; // We want to use the texture's format if it isn't supplied or if the requested format matches the guest format then we want to use the host format just in case it is host incompatible and the host format differs from the guest format

        if ((pFormat->vkAspect & format->vkAspect) == vk::ImageAspectFlagBits{}) {
            pFormat = format; // If the requested format doesn't share any aspects then fallback to the texture's format in the hope it's more likely to function
            range.aspectMask = format->Aspect(mapping.r == vk::ComponentSwizzle::eR);
        }
        if (!range.aspectMask || (range.aspectMask & ~format->vkAspect))
            throw exception("Texture view has unsupported image aspects");

        // Workaround to avoid aliasing when sampling from a BGRA texture with a RGBA view and a mapping to counteract that
        // TODO: drop this after new texture manager
        bool isRgbaBgraPair{
            (pFormat == format::R8G8B8A8Unorm && format == format::B8G8R8A8Unorm) ||
            (pFormat == format::R8G8B8A8Srgb && format == format::B8G8R8A8Srgb)
        };
        if (isRgbaBgraPair && mapping == vk::ComponentMapping{vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eA}) {
            pFormat = format;
            mapping = vk::ComponentMapping{};
        }

        if (!ViewFormatAdmitted(pFormat->vkFormat)) {
            if (!format->IsCompatible(*pFormat))
                throw exception("Incompatible texture view formats {} and {}", vk::to_string(format->vkFormat), vk::to_string(pFormat->vkFormat));
            if (context)
                throw TextureBackingPromotionRequest{shared_from_this(), pFormat->vkFormat};
            // No executor context: the caller holds the texture lock, excluding queued uses.
            PromoteBackingForView(pFormat->vkFormat);
        }

        return std::make_shared<TextureView>(shared_from_this(), type, range, pFormat, mapping);
    }

    void Texture::CopyFrom(std::shared_ptr<Texture> source, vk::Semaphore waitSemaphore, vk::Semaphore signalSemaphore, texture::Format srcFormat, const vk::ImageSubresourceRange &subresource) {
        if (cycle)
            cycle->WaitSubmit();
        if (source->cycle)
            source->cycle->WaitSubmit();

        WaitOnBacking();
        source->WaitOnBacking();
        WaitOnFence();

        if (source->layout == vk::ImageLayout::eUndefined)
            throw exception("Cannot copy from image with undefined layout");
        else if (source->dimensions != dimensions)
            throw exception("Cannot copy from image with different dimensions");

        TRACE_EVENT("gpu", "Texture::CopyFrom");

        auto submitFunc{[&](vk::Semaphore extraWaitSemaphore){
            boost::container::small_vector<vk::Semaphore, 2> waitSemaphores;
            if (waitSemaphore)
                waitSemaphores.push_back(waitSemaphore);

            if (extraWaitSemaphore)
                waitSemaphores.push_back(extraWaitSemaphore);

            auto active{gpu.scheduler.AllocateCommandBuffer()};
            auto copyCycle{active.GetFenceCycle()};
            const auto previousLayout{layout};
            try {
                // Retain both image owners before any GPU work can be submitted.
                copyCycle->AttachObjects(source, shared_from_this());
                active->begin(vk::CommandBufferBeginInfo{
                    .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
                });
                auto &commandBuffer{*active};
                auto sourceBacking{source->GetBacking()};
                if (source->layout != vk::ImageLayout::eTransferSrcOptimal) {
                    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                        .image = sourceBacking,
                        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                        .oldLayout = source->layout,
                        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .subresourceRange = subresource,
                        });
                }

                auto destinationBacking{GetBacking()};
                if (layout != vk::ImageLayout::eTransferDstOptimal) {
                    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
                        .image = destinationBacking,
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .oldLayout = layout,
                        .newLayout = vk::ImageLayout::eTransferDstOptimal,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .subresourceRange = subresource,
                        });

                    if (layout == vk::ImageLayout::eUndefined)
                        layout = vk::ImageLayout::eTransferDstOptimal;
                }

                vk::ImageSubresourceLayers subresourceLayers{
                    .aspectMask = subresource.aspectMask,
                    .mipLevel = subresource.baseMipLevel,
                    .baseArrayLayer = subresource.baseArrayLayer,
                    .layerCount = subresource.layerCount == VK_REMAINING_ARRAY_LAYERS ? layerCount - subresource.baseArrayLayer : subresource.layerCount,
                    };
                for (; subresourceLayers.mipLevel < (subresource.levelCount == VK_REMAINING_MIP_LEVELS ? levelCount - subresource.baseMipLevel : subresource.levelCount); subresourceLayers.mipLevel++) {
                    if (srcFormat != format) {
                        commandBuffer.blitImage(sourceBacking, vk::ImageLayout::eTransferSrcOptimal, destinationBacking, vk::ImageLayout::eTransferDstOptimal, vk::ImageBlit{
                                .srcSubresource = subresourceLayers,
                                .srcOffsets = std::array<vk::Offset3D, 2>{
                                    vk::Offset3D{0, 0, 0},
                                    vk::Offset3D{static_cast<i32>(dimensions.width),
                                                 static_cast<i32>(dimensions.height),
                                                 static_cast<i32>(subresourceLayers.layerCount)}
                                },
                                .dstSubresource = subresourceLayers,
                                .dstOffsets = std::array<vk::Offset3D, 2>{
                                    vk::Offset3D{0, 0, 0},
                                    vk::Offset3D{static_cast<i32>(dimensions.width),
                                                 static_cast<i32>(dimensions.height),
                                                 static_cast<i32>(subresourceLayers.layerCount)}
                                }
                            }, vk::Filter::eLinear);
                    } else {
                        commandBuffer.copyImage(sourceBacking, vk::ImageLayout::eTransferSrcOptimal, destinationBacking, vk::ImageLayout::eTransferDstOptimal, vk::ImageCopy{
                            .srcSubresource = subresourceLayers,
                            .dstSubresource = subresourceLayers,
                            .extent = dimensions,
                        });
                    }
                }

                if (layout != vk::ImageLayout::eTransferDstOptimal)
                    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
                        .image = destinationBacking,
                        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                        .newLayout = layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .subresourceRange = subresource,
                        });

                if (source->layout != vk::ImageLayout::eTransferSrcOptimal)
                    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
                        .image = sourceBacking,
                        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                        .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                        .newLayout = source->layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .subresourceRange = subresource,
                        });
                active->end();
                gpu.scheduler.SubmitCommandBuffer(*active, copyCycle, waitSemaphores, span<vk::Semaphore>{signalSemaphore});
            } catch (...) {
                copyCycle->Cancel();
                layout = previousLayout;
                throw;
            }
            return copyCycle;
        }};

        auto newCycle{[&]{
            if (source->cycle)
                return source->cycle->RecordSemaphoreWaitUsage(std::move(submitFunc));
            else
                return submitFunc({});
        }()};
        // A copy reads the source as well as writing the destination. Subsequent
        // CPU backing writes, promotion and guest readback must see this read,
        // even when there was no producer fence or its semaphore was already used.
        // Do not chain the old source cycle here: RecordSemaphoreWaitUsage can
        // already make its completion depend on this cycle to recycle a semaphore.
        source->cycle = newCycle;
        cycle = std::move(newCycle);
    }

    bool Texture::ValidateRenderPassUsage(u32 renderPassIndex, texture::RenderPassUsage renderPassUsage) {
        return lastRenderPassUsage == renderPassUsage || lastRenderPassIndex != renderPassIndex || lastRenderPassUsage == texture::RenderPassUsage::None;
    }

    void Texture::UpdateRenderPassUsage(u32 renderPassIndex, texture::RenderPassUsage renderPassUsage) {
        // Reject a read-only alias before mutating render-pass/write tracking.
        if (renderPassUsage == texture::RenderPassUsage::RenderTarget)
            RecordGpuWrite();

        lastRenderPassUsage = renderPassUsage;
        lastRenderPassIndex = renderPassIndex;

        if (renderPassUsage == texture::RenderPassUsage::RenderTarget) {
            everUsedAsRt = true;
            pendingStageMask = vk::PipelineStageFlagBits::eVertexShader |
                vk::PipelineStageFlagBits::eTessellationControlShader |
                vk::PipelineStageFlagBits::eTessellationEvaluationShader |
                vk::PipelineStageFlagBits::eGeometryShader |
                vk::PipelineStageFlagBits::eFragmentShader |
                vk::PipelineStageFlagBits::eComputeShader;
            readStageMask = {};
        } else if (renderPassUsage == texture::RenderPassUsage::None) {
            pendingStageMask = {};
            readStageMask = {};
        }
    }

    texture::RenderPassUsage Texture::GetLastRenderPassUsage() {
        return lastRenderPassUsage;
    }

    vk::PipelineStageFlags Texture::GetReadStageMask() {
        return readStageMask;
    }

    void Texture::PopulateReadBarrier(vk::PipelineStageFlagBits dstStage, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (!guest)
            return;

        readStageMask |= dstStage;

        if (!(pendingStageMask & dstStage))
            return;

        if (format->vkAspect & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil))
            srcStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
        else if (format->vkAspect & vk::ImageAspectFlagBits::eColor)
            srcStageMask |= vk::PipelineStageFlagBits::eColorAttachmentOutput;

        pendingStageMask &= ~dstStage;
        dstStageMask |= dstStage;
    }
}
