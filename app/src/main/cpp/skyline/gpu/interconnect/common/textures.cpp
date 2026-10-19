// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include <gpu/texture_manager.h>
#include <gpu/texture/format.h>
#include "textures.h"

namespace skyline::gpu::interconnect {
    void TexturePoolState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, texHeaderPool);
    }

    TexturePoolState::TexturePoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void TexturePoolState::Flush(InterconnectContext &ctx) {
        PurgeCaches();
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        mappingAddressSpace = &gmmu;
        mappingGeneration = gmmu.GetMappingGeneration();
        const u64 address{engine->texHeaderPool.offset};
        headerCount = static_cast<size_t>(engine->texHeaderPool.maximumIndex) + 1;
        const size_t size{headerCount * sizeof(TextureImageControl)};
        auto [mapping, offset]{gmmu.LookupBlock(address)};
        if (mapping.valid() && size <= mapping.size() - offset) {
            textureHeaders = mapping.subspan(offset, size).cast<TextureImageControl>();
        }
    }

    bool TexturePoolState::Refresh(InterconnectContext &ctx) {
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        return mappingAddressSpace != &gmmu || mappingGeneration != gmmu.GetMappingGeneration();
    }

    TextureImageControl *TexturePoolState::GetHeader(InterconnectContext &ctx, u32 index) {
        if (index >= headerCount)
            return nullptr;
        if (!textureHeaders.empty())
            return &textureHeaders[index];

        // A declared pool can have an unused unmapped tail or disjoint backing.
        // Resolve only this descriptor; never read through a physical segment.
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        const u64 address{u64{engine->texHeaderPool.offset} + static_cast<u64>(index) * sizeof(TextureImageControl)};
        const auto translated{gmmu.TranslateMappings(address, sizeof(TextureImageControl))};
        if (std::any_of(translated.begin(), translated.end(), [](const auto &mapping) {
                return mapping.kind == MemoryMappingKind::Unmapped;
            })) {
            LOGW("Unmapped texture header in pool: 0x{:X}", address);
            return nullptr;
        }
        if (translated.size() == 1 && translated.front().kind == MemoryMappingKind::Resident)
            return reinterpret_cast<TextureImageControl *>(translated.front().memory.data());

        // The bounded gather also implements zero reads for genuinely sparse
        // descriptor bytes; it is needed only when no direct alias is possible.
        splitHeader = gmmu.Read<TextureImageControl>(address);
        return &splitHeader;
    }

    void TexturePoolState::PurgeCaches() {
        textureHeaders = span<TextureImageControl>{};
        headerCount = 0;
        mappingAddressSpace = nullptr;
        mappingGeneration = 0;
    }

    Textures::Textures(DirtyManager &manager, const TexturePoolState::EngineRegisters &engine) : texturePool{manager, engine} {}

    void Textures::MarkAllDirty() {
        texturePool.MarkDirty(true);
    }

    static texture::Format ConvertTicFormat(TextureImageControl::FormatWord format, bool srgb) {
        using TIC = TextureImageControl;
        #define TIC_FORMAT(fmt, compR, compG, compB, compA, srgb) \
                TIC::FormatWord{ .format = TIC::ImageFormat::fmt,          \
                                 .componentR = TIC::ImageComponent::compR, \
                                 .componentG = TIC::ImageComponent::compG, \
                                 .componentB = TIC::ImageComponent::compB, \
                                 .componentA = TIC::ImageComponent::compA, \
                                 ._pad_ = srgb }.Raw() // Reuse _pad_ to store if the texture is sRGB

        // For formats where all components are of the same type
        #define TIC_FORMAT_ST(format, component) \
                TIC_FORMAT(format, component, component, component, component, false)

        #define TIC_FORMAT_ST_SRGB(format, component) \
                TIC_FORMAT(format, component, component, component, component, true)

        #define TIC_FORMAT_CASE(ticFormat, skFormat, componentR, componentG, componentB, componentA)  \
                case TIC_FORMAT(ticFormat, componentR, componentG, componentB, componentA, false): \
                    return format::skFormat

        #define TIC_FORMAT_CASE_ST(ticFormat, skFormat, component)  \
                case TIC_FORMAT_ST(ticFormat, component): \
                    return format::skFormat ## component

        #define TIC_FORMAT_CASE_ST_SRGB(ticFormat, skFormat, component)  \
                case TIC_FORMAT_ST_SRGB(ticFormat, component): \
                    return format::skFormat ## Srgb

        #define TIC_FORMAT_CASE_NORM(ticFormat, skFormat)  \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Unorm); \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Snorm)

        #define TIC_FORMAT_CASE_INT(ticFormat, skFormat)  \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Uint); \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Sint)

        #define TIC_FORMAT_CASE_NORM_INT(ticFormat, skFormat) \
                TIC_FORMAT_CASE_NORM(ticFormat, skFormat); \
                TIC_FORMAT_CASE_INT(ticFormat, skFormat)

        #define TIC_FORMAT_CASE_NORM_INT_FLOAT(ticFormat, skFormat) \
                TIC_FORMAT_CASE_NORM_INT(ticFormat, skFormat); \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Float)

        #define TIC_FORMAT_CASE_INT_FLOAT(ticFormat, skFormat) \
                TIC_FORMAT_CASE_INT(ticFormat, skFormat); \
                TIC_FORMAT_CASE_ST(ticFormat, skFormat, Float)

        // Ignore the swizzle components of the format word
        format._pad_ = srgb; // Reuse the _pad_ field to store the srgb flag
        switch ((format.Raw() & TextureImageControl::FormatWord::FormatColorComponentPadMask)) {
            TIC_FORMAT_CASE_NORM_INT(R8, R8);

            TIC_FORMAT_CASE_NORM_INT_FLOAT(R16, R16);
            TIC_FORMAT_CASE_ST(D16, D16, Unorm);
            TIC_FORMAT_CASE_NORM_INT(R8G8, R8G8);
            TIC_FORMAT_CASE_ST(B5G6R5, B5G6R5, Unorm);
            TIC_FORMAT_CASE_ST(R4G4B4A4, R4G4B4A4, Unorm);
            TIC_FORMAT_CASE_ST(A1B5G5R5, A1B5G5R5, Unorm);

            TIC_FORMAT_CASE_INT_FLOAT(R32, R32);
            TIC_FORMAT_CASE_ST(D32, D32, Float);
            TIC_FORMAT_CASE_NORM_INT_FLOAT(R16G16, R16G16);
            TIC_FORMAT_CASE(R8G24, S8UintD24Unorm, Uint, Unorm, Unorm, Unorm);
            TIC_FORMAT_CASE(S8D24, S8UintD24Unorm, Uint, Unorm, Uint, Uint);
            TIC_FORMAT_CASE(S8D24, S8UintD24Unorm, Uint, Unorm, Unorm, Unorm);
            TIC_FORMAT_CASE(D24S8, D24UnormS8Uint, Unorm, Uint, Uint, Uint);

            TIC_FORMAT_CASE_ST(B10G11R11, B10G11R11, Float);
            TIC_FORMAT_CASE_NORM_INT(A8B8G8R8, R8G8B8A8);
            TIC_FORMAT_CASE_ST_SRGB(A8B8G8R8, R8G8B8A8, Unorm);
            TIC_FORMAT_CASE_NORM_INT(A2B10G10R10, A2B10G10R10);
            TIC_FORMAT_CASE_ST(E5B9G9R9, E5B9G9R9, Float);

            TIC_FORMAT_CASE_ST(BC1, BC1, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(BC1, BC1, Unorm);
            TIC_FORMAT_CASE_NORM(BC4, BC4);
            TIC_FORMAT_CASE_INT_FLOAT(R32G32, R32G32);
            TIC_FORMAT_CASE(D32S8, D32FloatS8Uint, Float, Uint, Uint, Unorm);
            TIC_FORMAT_CASE(D32S8, D32FloatS8Uint, Float, Uint, Unorm, Unorm);
            TIC_FORMAT_CASE(R32B24G8, D32FloatS8Uint, Float, Uint, Unorm, Unorm);

            TIC_FORMAT_CASE_NORM_INT_FLOAT(R16G16B16A16, R16G16B16A16);

            TIC_FORMAT_CASE_ST(Astc4x4, Astc4x4, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc4x4, Astc4x4, Unorm);
            TIC_FORMAT_CASE_ST(Astc5x4, Astc5x4, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc5x4, Astc5x4, Unorm);
            TIC_FORMAT_CASE_ST(Astc5x5, Astc5x5, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc5x5, Astc5x5, Unorm);
            TIC_FORMAT_CASE_ST(Astc6x5, Astc6x5, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc6x5, Astc6x5, Unorm);
            TIC_FORMAT_CASE_ST(Astc6x6, Astc6x6, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc6x6, Astc6x6, Unorm);
            TIC_FORMAT_CASE_ST(Astc8x5, Astc8x5, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc8x5, Astc8x5, Unorm);
            TIC_FORMAT_CASE_ST(Astc8x6, Astc8x6, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc8x6, Astc8x6, Unorm);
            TIC_FORMAT_CASE_ST(Astc8x8, Astc8x8, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc8x8, Astc8x8, Unorm);
            TIC_FORMAT_CASE_ST(Astc10x5, Astc10x5, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc10x5, Astc10x5, Unorm);
            TIC_FORMAT_CASE_ST(Astc10x6, Astc10x6, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc10x6, Astc10x6, Unorm);
            TIC_FORMAT_CASE_ST(Astc10x8, Astc10x8, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc10x8, Astc10x8, Unorm);
            TIC_FORMAT_CASE_ST(Astc10x10, Astc10x10, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc10x10, Astc10x10, Unorm);
            TIC_FORMAT_CASE_ST(Astc12x10, Astc12x10, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(Astc12x10, Astc12x10, Unorm);
            TIC_FORMAT_CASE_ST(Astc12x12, Astc12x12, Unorm);
	        TIC_FORMAT_CASE_ST_SRGB(Astc12x12, Astc12x12, Unorm);

            TIC_FORMAT_CASE_ST(BC2, BC2, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(BC2, BC2, Unorm);
            TIC_FORMAT_CASE_ST(BC3, BC3, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(BC3, BC3, Unorm);
            TIC_FORMAT_CASE_NORM(BC5, BC5);
            TIC_FORMAT_CASE(Bc6HUfloat, Bc6HUfloat, Float, Float, Float, Float);
            TIC_FORMAT_CASE(Bc6HSfloat, Bc6HSfloat, Float, Float, Float, Float);
            TIC_FORMAT_CASE_ST(BC7, BC7, Unorm);
            TIC_FORMAT_CASE_ST_SRGB(BC7, BC7, Unorm);

            TIC_FORMAT_CASE_INT_FLOAT(R32G32B32A32, R32G32B32A32);

            default:
                if (format.Raw())
                    LOGE("Cannot translate TIC format: 0x{:X}", static_cast<u32>(format.Raw()));
                return {};
        }

        #undef TIC_FORMAT
        #undef TIC_FORMAT_ST
        #undef TIC_FORMAT_CASE
        #undef TIC_FORMAT_CASE_ST
        #undef TIC_FORMAT_CASE_NORM
        #undef TIC_FORMAT_CASE_INT
        #undef TIC_FORMAT_CASE_NORM_INT
        #undef TIC_FORMAT_CASE_NORM_INT_FLOAT
    }

    static bool IsIntegerTicFormat(texture::Format format) {
        return format == format::R8Uint ||
               format == format::R8Sint ||
               format == format::R16Uint ||
               format == format::R16Sint ||
               format == format::R8G8Uint ||
               format == format::R8G8Sint ||
               format == format::R32Uint ||
               format == format::R32Sint ||
               format == format::R16G16Uint ||
               format == format::R16G16Sint ||
               format == format::R8G8B8A8Uint ||
               format == format::R8G8B8A8Sint ||
               format == format::A2B10G10R10Uint ||
               format == format::A2B10G10R10Sint ||
               format == format::R32G32Uint ||
               format == format::R32G32Sint ||
               format == format::R16G16B16A16Uint ||
               format == format::R16G16B16A16Sint ||
               format == format::R32G32B32A32Uint ||
               format == format::R32G32B32A32Sint;
    }

    static vk::ComponentMapping ConvertTicSwizzleMapping(TextureImageControl::FormatWord format, vk::ComponentMapping swizzleMapping) {
        auto convertComponentSwizzle{[swizzleMapping](TextureImageControl::ImageSwizzle swizzle) {
            switch (swizzle) {
                case TextureImageControl::ImageSwizzle::R:
                    return swizzleMapping.r;
                case TextureImageControl::ImageSwizzle::G:
                    return swizzleMapping.g;
                case TextureImageControl::ImageSwizzle::B:
                    return swizzleMapping.b;
                case TextureImageControl::ImageSwizzle::A:
                    return swizzleMapping.a;
                case TextureImageControl::ImageSwizzle::Zero:
                    return vk::ComponentSwizzle::eZero;
                case TextureImageControl::ImageSwizzle::OneFloat:
                case TextureImageControl::ImageSwizzle::OneInt:
                    return vk::ComponentSwizzle::eOne;
                default:
                    throw exception("Invalid swizzle: {:X}", static_cast<u32>(swizzle));
            }
        }};

        return vk::ComponentMapping{
            .r = convertComponentSwizzle(format.swizzleX),
            .g = convertComponentSwizzle(format.swizzleY),
            .b = convertComponentSwizzle(format.swizzleZ),
            .a = convertComponentSwizzle(format.swizzleW)
        };
    }

    static std::shared_ptr<TextureView> CreateNullTexture(InterconnectContext &ctx) {
        constexpr texture::Format NullImageFormat{format::R8G8B8A8Unorm};
        constexpr texture::Dimensions NullImageDimensions{1, 1, 1};
        constexpr vk::ImageLayout NullImageInitialLayout{vk::ImageLayout::eUndefined};
        constexpr vk::ImageTiling NullImageTiling{vk::ImageTiling::eOptimal};
        constexpr vk::ImageCreateFlags NullImageFlags{};
        constexpr vk::ImageUsageFlags NullImageUsage{vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage};

        auto vkImage{ctx.gpu.memory.AllocateImage(
            {
                .flags = NullImageFlags,
                .imageType = vk::ImageType::e2D,
                .format = NullImageFormat->vkFormat,
                .extent = NullImageDimensions,
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = NullImageTiling,
                .usage = NullImageUsage,
                .sharingMode = vk::SharingMode::eExclusive,
                .queueFamilyIndexCount = 1,
                .pQueueFamilyIndices = &ctx.gpu.vkQueueFamilyIndex,
                .initialLayout = NullImageInitialLayout
            }
        )};

        auto nullTexture{std::make_shared<Texture>(ctx.gpu, std::move(vkImage), NullImageDimensions, NullImageFormat, NullImageInitialLayout, NullImageTiling, NullImageFlags, NullImageUsage)};
        nullTexture->TransitionLayout(vk::ImageLayout::eGeneral);
        return nullTexture->GetView(vk::ImageViewType::e2D, vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        });;
    }

    TextureView *Textures::GetTexture(InterconnectContext &ctx, u32 index, Shader::TextureType shaderType) {
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        const auto generation{gmmu.GetMappingGeneration()};
        if (mappingAddressSpace != &gmmu || mappingGeneration != generation) {
            // A TIC describes GPU virtual addresses. Identical TIC bytes do not
            // imply identical physical or sparse backing after a GMMU remap.
            textureHeaderCache.clear();
            textureHeaderStore.clear();
            texturePool.MarkDirty(true);
            mappingAddressSpace = &gmmu;
            mappingGeneration = generation;
        }
        auto &pool{texturePool.UpdateGet(ctx)};
        if (textureHeaderCache.size() != pool.headerCount) {
            textureHeaderCache.resize(pool.headerCount);
            std::fill(textureHeaderCache.begin(), textureHeaderCache.end(), CacheEntry{});
        } else if (pool.headerCount > index && textureHeaderCache[index].view) {
            auto &cached{textureHeaderCache[index]};
            if (cached.sequenceNumber == ctx.channelCtx.channelSequenceNumber)
                return cached.view;
        }

        auto header{pool.GetHeader(ctx, index)};
        if (!header) {
            if (!nullTextureView)
                nullTextureView = CreateNullTexture(ctx);

            return nullTextureView.get();
        }

        TextureImageControl &textureHeader{*header};
        auto &cached{textureHeaderCache[index]};
        if (cached.view && cached.tic == textureHeader && !cached.view->texture->replaced) {
            cached.sequenceNumber = ctx.channelCtx.channelSequenceNumber;
            return cached.view;
        }
        auto &texture{textureHeaderStore[textureHeader]};

        if (!texture || texture->texture->replaced) {
            // If the entry didn't exist prior then we need to convert the TIC to a GuestTexture
            GuestTexture guest{};
            if (auto format{ConvertTicFormat(textureHeader.formatWord, textureHeader.isSrgb)}) {
                guest.format = format;
            } else {
                if (!nullTextureView)
                    nullTextureView = CreateNullTexture(ctx);

                return nullTextureView.get();
            }

            guest.aspect = guest.format->Aspect(textureHeader.formatWord.swizzleX == TextureImageControl::ImageSwizzle::R);
            guest.swizzle = ConvertTicSwizzleMapping(textureHeader.formatWord, guest.format->swizzleMapping);

            constexpr size_t CubeFaceCount{6};

            guest.baseArrayLayer = static_cast<u16>(textureHeader.BaseLayer());
            guest.dimensions = texture::Dimensions(textureHeader.widthMinusOne + 1, textureHeader.heightMinusOne + 1, 1);
            u16 depth{static_cast<u16>(textureHeader.depthMinusOne + 1)};

            guest.mipLevelCount = textureHeader.mipMaxLevels + 1;
            guest.viewMipBase = textureHeader.viewConfig.mipMinLevel;
            guest.viewMipCount = textureHeader.viewConfig.mipMaxLevel - textureHeader.viewConfig.mipMinLevel + 1;

            switch (textureHeader.textureType) {
                case TextureImageControl::TextureType::e1D:
                    guest.viewType = shaderType == Shader::TextureType::ColorArray1D ? vk::ImageViewType::e1DArray : vk::ImageViewType::e1D;
                    guest.layerCount = 1;
                    break;
                case TextureImageControl::TextureType::e1DArray:
                    guest.viewType = vk::ImageViewType::e1DArray;
                    guest.layerCount = depth;
                    break;
                case TextureImageControl::TextureType::e1DBuffer:
                    throw exception("1D Buffers are not supported");

                case TextureImageControl::TextureType::e2DNoMipmap:
                    guest.mipLevelCount = 1;
                    guest.viewMipBase = 0;
                    guest.viewMipCount = 1;
                case TextureImageControl::TextureType::e2D:
                    guest.viewType = shaderType == Shader::TextureType::ColorArray2D ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
                    guest.layerCount = 1;
                    break;
                case TextureImageControl::TextureType::e2DArray:
                    guest.viewType = vk::ImageViewType::e2DArray;
                    guest.layerCount = depth;
                    break;

                case TextureImageControl::TextureType::e3D:
                    guest.viewType = vk::ImageViewType::e3D;
                    guest.layerCount = 1;
                    guest.dimensions.depth = depth;
                    break;

                case TextureImageControl::TextureType::eCube:
                    guest.viewType = shaderType == Shader::TextureType::ColorArrayCube ? vk::ImageViewType::eCubeArray : vk::ImageViewType::eCube;
                    guest.layerCount = CubeFaceCount;
                    break;
                case TextureImageControl::TextureType::eCubeArray:
                    guest.viewType = vk::ImageViewType::eCubeArray;
                    guest.layerCount = depth * CubeFaceCount;
                    break;
            }

            if (textureHeader.headerType == TextureImageControl::HeaderType::Pitch) {
                guest.tileConfig = {
                    .mode = texture::TileMode::Pitch,
                    .pitch = static_cast<u32>(textureHeader.tileConfig.pitchHigh) << TextureImageControl::TileConfig::PitchAlignmentBits,
                };
            } else if (textureHeader.headerType == TextureImageControl::HeaderType::BlockLinear) {
                guest.tileConfig = {
                    .mode = texture::TileMode::Block,
                    .blockHeight = static_cast<u8>(1U << textureHeader.tileConfig.tileHeightGobsLog2),
                    .blockDepth = static_cast<u8>(1U << textureHeader.tileConfig.tileDepthGobsLog2),
                };
            } else {
                throw exception("Unsupported TIC Header Type: {}", static_cast<u32>(textureHeader.headerType));
            }

            if (guest.tileConfig.mode == texture::TileMode::Block && guest.mipLevelCount == 1 && !guest.baseArrayLayer &&
                guest.GetImageType() == vk::ImageType::e2D && guest.tileConfig.blockDepth == 1 &&
                guest.format->vkAspect == vk::ImageAspectFlagBits::eColor) {
                // TIC sampled surfaces auto-size Y blocks, including at level zero.
                // Limit this to layouts supported by the lossless alias bridge.
                // Cache the declared allocation first: array layers still advance by
                // the header's stride even when sampling uses fewer Y GOBs.
                guest.GetLayerStride();
                const auto heightBlocks{util::DivideCeil<u32>(guest.dimensions.height, guest.format->blockHeight)};
                constexpr u32 GobHeight{8};
                while (guest.tileConfig.blockHeight > 1 && heightBlocks <= (guest.tileConfig.blockHeight >> 1) * GobHeight)
                    guest.tileConfig.blockHeight >>= 1;
            }

            guest.SetMappings(ctx.channelCtx.asCtx->gmmu, textureHeader.Iova());
            if (!guest.MappingsValid()) {
                LOGW("Unmapped texture in pool: 0x{:X}", textureHeader.Iova());
                if (!nullTextureView)
                    nullTextureView = CreateNullTexture(ctx);

                return nullTextureView.get();
            }
            texture = ctx.gpu.texture.FindOrCreate(guest, ctx.executor.tag);
        }

        textureHeaderCache[index] = {textureHeader, texture.get(), ctx.channelCtx.channelSequenceNumber};
        return texture.get();
    }

    Shader::TextureType Textures::GetTextureType(InterconnectContext &ctx, u32 index) {
        auto &pool{texturePool.UpdateGet(ctx)};
        auto header{pool.GetHeader(ctx, index)};
        if (!header) {
            if (index >= pool.headerCount)
                LOGW("Texture type requested for out-of-range TIC index {}", index);
            return Shader::TextureType::Color2D;
        }

        switch (header->textureType) {
            case TextureImageControl::TextureType::e1D:
                return Shader::TextureType::Color1D;
            case TextureImageControl::TextureType::e1DArray:
                return Shader::TextureType::ColorArray1D;
            case TextureImageControl::TextureType::e1DBuffer:
                return Shader::TextureType::Buffer;
            case TextureImageControl::TextureType::e2DNoMipmap:
            case TextureImageControl::TextureType::e2D:
                return Shader::TextureType::Color2D;
            case TextureImageControl::TextureType::e2DArray:
                return Shader::TextureType::ColorArray2D;
            case TextureImageControl::TextureType::e3D:
                return Shader::TextureType::Color3D;
            case TextureImageControl::TextureType::eCube:
                return Shader::TextureType::ColorCube;
            case TextureImageControl::TextureType::eCubeArray:
                return Shader::TextureType::ColorArrayCube;
            }

        throw exception("Invalid TIC texture type: {}", static_cast<u32>(header->textureType));
    }

    bool Textures::IsTexturePixelFormatInteger(InterconnectContext &ctx, u32 index) {
        auto &pool{texturePool.UpdateGet(ctx)};
        auto header{pool.GetHeader(ctx, index)};
        if (!header) {
            if (index >= pool.headerCount)
                LOGW("Texture pixel format requested for out-of-range TIC index {}", index);
            return false;
        }

        const auto &textureHeader{*header};
        const auto format{ConvertTicFormat(textureHeader.formatWord, textureHeader.isSrgb)};
        if (!format) {
            LOGW("Texture pixel format requested for unsupported TIC index {}", index);
            return false;
        }

        return IsIntegerTicFormat(format);
    }

    vk::raii::BufferView *Textures::GetOrCreateTexelBufferView(
        InterconnectContext &ctx, CachedMappedBufferView &cachedView, vk::Format format) {
        auto binding{cachedView.view.GetBinding(ctx.gpu)};
        Textures::TexelBufferViewKey key{
            .buffer = static_cast<VkBuffer>(binding.buffer),
            .offset = binding.offset,
            .range = binding.size,
            .format = static_cast<VkFormat>(format),
        };

        auto &bufferView{texelBufferViewStore[key]};
        if (!bufferView) {
            bufferView = std::make_unique<vk::raii::BufferView>(ctx.gpu.vkDevice, vk::BufferViewCreateInfo{
                .buffer = binding.buffer,
                .format = format,
                .offset = binding.offset,
                .range = binding.size,
            });
        }
        return bufferView.get();
    }

    vk::raii::BufferView *Textures::GetTextureBufferView(InterconnectContext &ctx, u32 index, CachedMappedBufferView &cachedView) {
        auto header{texturePool.UpdateGet(ctx).GetHeader(ctx, index)};
        if (!header)
            return nullptr;

        TextureImageControl &textureHeader{*header};

        auto format{ConvertTicFormat(textureHeader.formatWord, textureHeader.isSrgb)};
        if (!format) {
            LOGW("Texture buffer: unsupported format for TIC index {}", index);
            return nullptr;
        }

        size_t elementCount{
            (static_cast<size_t>(textureHeader.tileConfig.widthMinusOne_16_31) << 16) |
            textureHeader.widthMinusOne
        };
        elementCount++;
        size_t sizeBytes{elementCount * format->bpb};

        cachedView.Update(ctx, textureHeader.Iova(), sizeBytes);
        if (!cachedView.view) {
            LOGW("Unmapped texture buffer in pool: 0x{:X}", textureHeader.Iova());
            return nullptr;
        }

        return GetOrCreateTexelBufferView(ctx, cachedView, format->vkFormat);
    }

    static std::optional<std::pair<vk::Format, u8>> ConvertShaderImageFormat(Shader::ImageFormat format) {
        switch (format) {
            case Shader::ImageFormat::Typeless:
                return std::pair{vk::Format::eR32Uint, u8{4}};
            case Shader::ImageFormat::R8_UINT:
                return std::pair{vk::Format::eR8Uint, u8{1}};
            case Shader::ImageFormat::R8_SINT:
                return std::pair{vk::Format::eR8Sint, u8{1}};
            case Shader::ImageFormat::R16_UINT:
                return std::pair{vk::Format::eR16Uint, u8{2}};
            case Shader::ImageFormat::R16_SINT:
                return std::pair{vk::Format::eR16Sint, u8{2}};
            case Shader::ImageFormat::R32_UINT:
                return std::pair{vk::Format::eR32Uint, u8{4}};
            case Shader::ImageFormat::R32G32_UINT:
                return std::pair{vk::Format::eR32G32Uint, u8{8}};
            case Shader::ImageFormat::R32G32B32A32_UINT:
                return std::pair{vk::Format::eR32G32B32A32Uint, u8{16}};
            default:
                return std::nullopt;
        }
    }

    vk::raii::BufferView *Textures::GetImageBufferView(InterconnectContext &ctx, u32 index, Shader::ImageFormat shaderFormat, CachedMappedBufferView &cachedView) {
        auto header{texturePool.UpdateGet(ctx).GetHeader(ctx, index)};
        if (!header)
            return nullptr;

        TextureImageControl &textureHeader{*header};

        auto format{ConvertShaderImageFormat(shaderFormat)};
        if (!format) {
            LOGW("Image buffer: unsupported shader format {}", static_cast<u32>(shaderFormat));
            return nullptr;
        }

        size_t elementCount{
            (static_cast<size_t>(textureHeader.tileConfig.widthMinusOne_16_31) << 16) |
            textureHeader.widthMinusOne
        };
        elementCount++;
        size_t sizeBytes{elementCount * format->second};

        cachedView.Update(ctx, textureHeader.Iova(), sizeBytes);
        if (!cachedView.view) {
            LOGW("Unmapped image buffer in pool: 0x{:X}", textureHeader.Iova());
            return nullptr;
        }

        return GetOrCreateTexelBufferView(ctx, cachedView, format->first);
    }
}
