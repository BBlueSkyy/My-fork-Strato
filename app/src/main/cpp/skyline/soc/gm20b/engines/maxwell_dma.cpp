// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/yuzu/)

#include <gpu/interconnect/command_executor.h>
#include <gpu/texture/format.h>
#include <gpu/texture/layout.h>
#include <soc.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "maxwell_dma.h"

namespace skyline::soc::gm20b::engine {
    MaxwellDma::MaxwellDma(const DeviceState &state, ChannelContext &channelCtx)
        : channelCtx{channelCtx},
          syncpoints{state.soc->host1x.syncpoints},
          interconnect{*state.gpu, channelCtx},
          copyCache() {}

    __attribute__((always_inline)) void MaxwellDma::CallMethod(u32 method, u32 argument) {
        LOGV("Called method in Maxwell DMA: 0x{:X} args: 0x{:X}", method, argument);

        HandleMethod(method, argument);
    }

    void MaxwellDma::HandleMethod(u32 method, u32 argument) {
        registers.raw[method] = argument;

        if (method == ENGINE_OFFSET(launchDma))
            LaunchDma();
    }

    void MaxwellDma::LaunchDma() {
        DmaCopy();

        ReleaseSemaphore();
    }

    void MaxwellDma::DmaCopy() {
        if (registers.launchDma->multiLineEnable) {
            if (registers.launchDma->remapEnable) [[unlikely]] {
                // Remap is handled entirely through the GMMU read/write path (see CopyRemapMultiLine),
                // it never touches the Vulkan-backed interconnect buffers, so we don't need to Submit() here
                if (registers.launchDma->srcMemoryLayout == Registers::LaunchDma::MemoryLayout::Pitch &&
                    registers.launchDma->dstMemoryLayout == Registers::LaunchDma::MemoryLayout::Pitch) {
                    CopyRemapMultiLine();
                } else {
                    // TODO: BlockLinear surfaces with remap enabled (e.g. compressed texture uploads with
                    // component padding) aren't handled yet, materialize into a linear scratch buffer via
                    // PerformRemap() and feed it through CopyBlockLinearToPitch/CopyPitchToBlockLinear
                    LOGW("Remapped DMA copies involving BlockLinear surfaces are unimplemented!");
                }
                return;
            }

            channelCtx.executor.Submit();

            if (registers.launchDma->srcMemoryLayout == registers.launchDma->dstMemoryLayout) [[unlikely]] {
                // Pitch to Pitch copy
                if (registers.launchDma->srcMemoryLayout == Registers::LaunchDma::MemoryLayout::Pitch) [[likely]] {
                    CopyPitchToPitch();
                } else {
                    LOGW("BlockLinear to BlockLinear DMA copies are unimplemented!");
                }
            } else if (registers.launchDma->srcMemoryLayout == Registers::LaunchDma::MemoryLayout::BlockLinear) {
                CopyBlockLinearToPitch();
            } else [[likely]] {
                CopyPitchToBlockLinear();
            }
        } else {
            // 1D copy
            // TODO: implement swizzled 1D copies based on VMM 'kind'
            LOGD("src: 0x{:X} dst: 0x{:X} size: 0x{:X}", u64{*registers.offsetIn}, u64{*registers.offsetOut}, *registers.lineLengthIn);

            if (registers.launchDma->remapEnable) [[unlikely]] {
                // Fast path: a pure broadcast fill of a single 4-byte constant across the whole
                // destination can still go through the Vulkan-backed Clear(), it's both correct
                // and much faster than the generic GMMU-based path below for the common case of
                // clearing a buffer/render target region.
                auto &remap{*registers.remapComponents};
                if ((remap.dstX == Registers::RemapComponents::Swizzle::ConstA) &&
                    (remap.dstY == Registers::RemapComponents::Swizzle::ConstA) &&
                    (remap.dstZ == Registers::RemapComponents::Swizzle::ConstA) &&
                    (remap.dstW == Registers::RemapComponents::Swizzle::ConstA) &&
                    (remap.ComponentSize() == 4)) {
                    size_t dstSize{*registers.lineLengthIn * remap.NumDstComponents() * remap.ComponentSize()};
                    auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetOut, dstSize)};
                    for (auto mapping : dstMappings)
                        interconnect.Clear(mapping, *registers.remapConstA);
                } else {
                    // General case: constant fills with other components/sizes, source component
                    // swizzles (e.g. RGB8 -> RGBA8 upload), or a mix of both
                    CopyRemap1D();
                }
            } else {
                auto srcMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetIn, *registers.lineLengthIn)};
                auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetOut, *registers.lineLengthIn)};

                if (srcMappings.size() != 1 || dstMappings.size() != 1) [[unlikely]]
                    channelCtx.asCtx->gmmu.Copy(u64{*registers.offsetOut}, u64{*registers.offsetIn}, *registers.lineLengthIn);
                else
                    interconnect.Copy(dstMappings.front(), srcMappings.front());
            }
        }
    }

    bool MaxwellDma::RemapNeedsSource() {
        auto &remap{*registers.remapComponents};
        std::array<Registers::RemapComponents::Swizzle, 4> dstSwizzle{remap.dstX, remap.dstY, remap.dstZ, remap.dstW};
        u8 numDstComponents{remap.NumDstComponents()};

        for (u8 component{}; component < numDstComponents; component++) {
            switch (dstSwizzle[component]) {
                case Registers::RemapComponents::Swizzle::SrcX:
                case Registers::RemapComponents::Swizzle::SrcY:
                case Registers::RemapComponents::Swizzle::SrcZ:
                case Registers::RemapComponents::Swizzle::SrcW:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    bool MaxwellDma::RemapNeedsDestinationPreserve() {
        auto &remap{*registers.remapComponents};
        std::array<Registers::RemapComponents::Swizzle, 4> dstSwizzle{remap.dstX, remap.dstY, remap.dstZ, remap.dstW};
        u8 numDstComponents{remap.NumDstComponents()};

        for (u8 component{}; component < numDstComponents; component++)
            if (dstSwizzle[component] == Registers::RemapComponents::Swizzle::NoWrite)
                return true;
        return false;
    }

    /**
     * @brief Applies the LAUNCH_DMA remap swizzle to `elementCount` elements, reading from `src`
     * (may be `nullptr` if the swizzle table doesn't reference any source component, e.g. a pure
     * constant fill) and writing fully swizzled elements into `dst`.
     * @note `dst` must already contain the current destination contents if any component is NoWrite,
     * see RemapNeedsDestinationPreserve()
     */
    void MaxwellDma::PerformRemap(u8 *dst, const u8 *src, size_t elementCount) {
        auto &remap{*registers.remapComponents};
        u8 componentSize{remap.ComponentSize()};
        u8 numSrcComponents{remap.NumSrcComponents()};
        u8 numDstComponents{remap.NumDstComponents()};

        std::array<Registers::RemapComponents::Swizzle, 4> dstSwizzle{remap.dstX, remap.dstY, remap.dstZ, remap.dstW};

        u32 constA{*registers.remapConstA};
        u32 constB{*registers.remapConstB};

        size_t srcStride{static_cast<size_t>(numSrcComponents) * componentSize};
        size_t dstStride{static_cast<size_t>(numDstComponents) * componentSize};

        for (size_t element{}; element < elementCount; element++) {
            const u8 *srcElem{src ? src + element * srcStride : nullptr};
            u8 *dstElem{dst + element * dstStride};

            for (u8 component{}; component < numDstComponents; component++) {
                u8 *dstComponent{dstElem + component * componentSize};

                switch (dstSwizzle[component]) {
                    case Registers::RemapComponents::Swizzle::SrcX:
                    case Registers::RemapComponents::Swizzle::SrcY:
                    case Registers::RemapComponents::Swizzle::SrcZ:
                    case Registers::RemapComponents::Swizzle::SrcW: {
                        auto srcIndex{static_cast<u8>(dstSwizzle[component])};
                        if (srcElem && srcIndex < numSrcComponents)
                            std::memcpy(dstComponent, srcElem + srcIndex * componentSize, componentSize);
                        else
                            // Reading a source component that wasn't supplied reads back as 0
                            std::memset(dstComponent, 0, componentSize);
                        break;
                    }

                    case Registers::RemapComponents::Swizzle::ConstA:
                        std::memcpy(dstComponent, &constA, componentSize);
                        break;

                    case Registers::RemapComponents::Swizzle::ConstB:
                        std::memcpy(dstComponent, &constB, componentSize);
                        break;

                    case Registers::RemapComponents::Swizzle::NoWrite:
                        // Destination bytes are left as-is, `dst` must have been pre-populated by the caller
                        break;
                }
            }
        }
    }

    void MaxwellDma::CopyRemap1D() {
        auto &remap{*registers.remapComponents};
        size_t elementCount{*registers.lineLengthIn};
        size_t dstBpp{static_cast<size_t>(remap.NumDstComponents()) * remap.ComponentSize()};
        size_t srcBpp{static_cast<size_t>(remap.NumSrcComponents()) * remap.ComponentSize()};
        size_t dstSize{elementCount * dstBpp};

        if (copyCache.size() < dstSize)
            copyCache.resize(dstSize);
        u8 *dstScratch{copyCache.data()};

        if (RemapNeedsDestinationPreserve())
            channelCtx.asCtx->gmmu.Read(dstScratch, u64{*registers.offsetOut}, dstSize);

        if (RemapNeedsSource()) {
            std::vector<u8> srcScratch(elementCount * srcBpp);
            channelCtx.asCtx->gmmu.Read(srcScratch.data(), u64{*registers.offsetIn}, srcScratch.size());
            PerformRemap(dstScratch, srcScratch.data(), elementCount);
        } else {
            PerformRemap(dstScratch, nullptr, elementCount);
        }

        channelCtx.asCtx->gmmu.Write(u64{*registers.offsetOut}, dstScratch, dstSize);
    }

    void MaxwellDma::CopyRemapMultiLine() {
        auto &remap{*registers.remapComponents};
        size_t elementsPerLine{*registers.lineLengthIn};
        size_t lines{*registers.lineCount};
        size_t dstBpp{static_cast<size_t>(remap.NumDstComponents()) * remap.ComponentSize()};
        size_t srcBpp{static_cast<size_t>(remap.NumSrcComponents()) * remap.ComponentSize()};
        size_t dstLineSize{elementsPerLine * dstBpp};
        size_t srcLineSize{elementsPerLine * srcBpp};

        bool needsSource{RemapNeedsSource()};
        bool needsDestinationPreserve{RemapNeedsDestinationPreserve()};

        if (copyCache.size() < dstLineSize)
            copyCache.resize(dstLineSize);
        u8 *dstScratch{copyCache.data()};

        std::vector<u8> srcScratch;
        if (needsSource)
            srcScratch.resize(srcLineSize);

        for (size_t line{}; line < lines; line++) {
            u64 srcLineOffset{u64{*registers.offsetIn} + line * *registers.pitchIn};
            u64 dstLineOffset{u64{*registers.offsetOut} + line * *registers.pitchOut};

            if (needsDestinationPreserve)
                channelCtx.asCtx->gmmu.Read(dstScratch, dstLineOffset, dstLineSize);

            if (needsSource) {
                channelCtx.asCtx->gmmu.Read(srcScratch.data(), srcLineOffset, srcLineSize);
                PerformRemap(dstScratch, srcScratch.data(), elementsPerLine);
            } else {
                PerformRemap(dstScratch, nullptr, elementsPerLine);
            }

            channelCtx.asCtx->gmmu.Write(dstLineOffset, dstScratch, dstLineSize);
        }
    }

    void MaxwellDma::HandleSplitCopy(TranslatedAddressRange srcMappings, TranslatedAddressRange dstMappings, size_t srcSize, size_t dstSize, auto copyCallback) {
        bool isSrcSplit{};
        u8 *src{srcMappings.front().data()}, *dst{dstMappings.front().data()};
        if (srcMappings.size() != 1) {
            if (copyCache.size() < srcSize)
                copyCache.resize(srcSize);

            src = copyCache.data();
            channelCtx.asCtx->gmmu.Read(src, u64{*registers.offsetIn}, srcSize);

            isSrcSplit = true;
        }
        if (dstMappings.size() != 1) {
            size_t offset{isSrcSplit ? srcSize : 0};

            if (copyCache.size() < (dstSize + offset))
            copyCache.resize(dstSize + offset);

            dst = copyCache.data() + offset;

            // If the destination is not entirely filled by the copy we copy it's current state in the cache to prevent clearing of other data.
            if (registers.launchDma->dstMemoryLayout == Registers::LaunchDma::MemoryLayout::BlockLinear)
                channelCtx.asCtx->gmmu.Read(dst, u64{*registers.offsetOut}, dstSize);
        }

        copyCallback(src, dst);

        if (dstMappings.size() != 1)
            channelCtx.asCtx->gmmu.Write(u64{*registers.offsetOut}, dst, dstSize);
    }

    void MaxwellDma::CopyPitchToPitch() {
        auto srcMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetIn, *registers.pitchIn * *registers.lineCount)};
        auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetOut, *registers.pitchOut * *registers.lineCount)};

        if (srcMappings.size() != 1 || dstMappings.size() != 1) [[unlikely]] {
            HandleSplitCopy(srcMappings, dstMappings, *registers.lineLengthIn, *registers.lineLengthIn, [&](u8 *src, u8 *dst) {
                // Both Linear, copy as is.
                if ((*registers.pitchIn == *registers.pitchOut) && (*registers.pitchIn == *registers.lineLengthIn))
                    std::memcpy(dst, src, *registers.lineLengthIn * *registers.lineCount);
                else
                    for (size_t linesToCopy{*registers.lineCount}, srcCopyOffset{}, dstCopyOffset{}; linesToCopy; --linesToCopy, srcCopyOffset += *registers.pitchIn, dstCopyOffset += *registers.pitchOut)
                        std::memcpy(dst + dstCopyOffset, src + srcCopyOffset, *registers.lineLengthIn);
            });
        } else [[likely]] {
            // Both Linear, copy as is.
            if ((*registers.pitchIn == *registers.pitchOut) && (*registers.pitchIn == *registers.lineLengthIn)) {
                std::memcpy(dstMappings.front().data(), srcMappings.front().data(), *registers.lineLengthIn * *registers.lineCount);
            } else {
                for (size_t linesToCopy{*registers.lineCount}, srcCopyOffset{}, dstCopyOffset{}; linesToCopy; --linesToCopy, srcCopyOffset += *registers.pitchIn, dstCopyOffset += *registers.pitchOut)
                    std::memcpy(dstMappings.front().subspan(dstCopyOffset).data(), srcMappings.front().subspan(srcCopyOffset).data(), *registers.lineLengthIn);
            }
        }
    }

    void MaxwellDma::CopyBlockLinearToPitch() {
        if (registers.srcSurface->blockSize.Width() != 1) [[unlikely]] {
            LOGE("Blocklinear surfaces with a non-one block width are unsupported on the Tegra X1: {}", registers.srcSurface->blockSize.Width());
            return;
        }

        gpu::texture::Dimensions srcDimensions{registers.srcSurface->width, registers.srcSurface->height, registers.srcSurface->depth};
        size_t srcLayerStride{gpu::texture::GetBlockLinearLayerSize(srcDimensions, 1, 1, 1, registers.srcSurface->blockSize.Height(), registers.srcSurface->blockSize.Depth())};
        size_t srcLayerAddress{*registers.offsetIn + (registers.srcSurface->layer * srcLayerStride)};

        // Get source address
        auto srcMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetIn, srcLayerStride)};

        gpu::texture::Dimensions dstDimensions{*registers.lineLengthIn, *registers.lineCount, registers.srcSurface->depth};
        size_t dstSize{*registers.pitchOut * dstDimensions.height * dstDimensions.depth}; // If remapping is not enabled there are only 1 bytes per pixel

        // Get destination address
        auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetOut, dstSize)};

        auto copyFunc{[&](u8 *src, u8 *dst) {
            if ((util::AlignDown(srcDimensions.width, 64) != util::AlignDown(dstDimensions.width, 64))
                || registers.srcSurface->origin.x || registers.srcSurface->origin.y) {
                gpu::texture::CopyBlockLinearToPitchSubrect(
                    dstDimensions, srcDimensions,
                    1, 1, 1, *registers.pitchOut,
                    registers.srcSurface->blockSize.Height(), registers.srcSurface->blockSize.Depth(),
                    src, dst,
                    registers.srcSurface->origin.x, registers.srcSurface->origin.y
                );
            } else [[likely]] {
                gpu::texture::CopyBlockLinearToPitch(
                    dstDimensions,
                    1, 1, 1, *registers.pitchOut,
                    registers.srcSurface->blockSize.Height(), registers.srcSurface->blockSize.Depth(),
                    src, dst
                );
            }
        }};

        LOGD("{}x{}x{}@0x{:X} -> {}x{}x{}@0x{:X}", srcDimensions.width, srcDimensions.height, srcDimensions.depth, srcLayerAddress, dstDimensions.width, dstDimensions.height, dstDimensions.depth, u64{*registers.offsetOut});

        if (srcMappings.size() != 1 || dstMappings.size() != 1) [[unlikely]]
            HandleSplitCopy(srcMappings, dstMappings, srcLayerStride, dstSize, copyFunc);
        else [[likely]]
            copyFunc(srcMappings.front().data(), dstMappings.front().data());
    }

    void MaxwellDma::CopyPitchToBlockLinear() {
        if (registers.dstSurface->blockSize.Width() != 1) [[unlikely]] {
            LOGE("Blocklinear surfaces with a non-one block width are unsupported on the Tegra X1: {}", registers.srcSurface->blockSize.Width());
            return;
        }

        gpu::texture::Dimensions srcDimensions{*registers.lineLengthIn, *registers.lineCount, registers.dstSurface->depth};
        size_t srcSize{*registers.pitchIn * srcDimensions.height * srcDimensions.depth}; // If remapping is not enabled there are only 1 bytes per pixel

        // Get source address
        auto srcMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetIn, srcSize)};

        gpu::texture::Dimensions dstDimensions{registers.dstSurface->width, registers.dstSurface->height, registers.dstSurface->depth};
        size_t dstLayerStride{gpu::texture::GetBlockLinearLayerSize(dstDimensions, 1, 1, 1, registers.dstSurface->blockSize.Height(), registers.dstSurface->blockSize.Depth())};
        size_t dstLayerAddress{*registers.offsetOut + (registers.dstSurface->layer * dstLayerStride)};

        // Get destination address
        auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(*registers.offsetOut, dstLayerStride)};

        LOGD("{}x{}x{}@0x{:X} -> {}x{}x{}@0x{:X}", srcDimensions.width, srcDimensions.height, srcDimensions.depth, u64{*registers.offsetIn}, dstDimensions.width, dstDimensions.height, dstDimensions.depth, dstLayerAddress);

        auto copyFunc{[&](u8 *src, u8 *dst) {
            if ((util::AlignDown(srcDimensions.width, 64) != util::AlignDown(dstDimensions.width, 64))
                || registers.dstSurface->origin.x || registers.dstSurface->origin.y) {
                gpu::texture::CopyPitchToBlockLinearSubrect(
                    srcDimensions, dstDimensions,
                    1, 1, 1, *registers.pitchIn,
                    registers.dstSurface->blockSize.Height(), registers.dstSurface->blockSize.Depth(),
                    src, dst,
                    registers.dstSurface->origin.x, registers.dstSurface->origin.y
                );
            } else [[likely]] {
                gpu::texture::CopyPitchToBlockLinear(
                    srcDimensions,
                    1, 1, 1, *registers.pitchIn,
                    registers.dstSurface->blockSize.Height(), registers.dstSurface->blockSize.Depth(),
                    src, dst
                );
            }
        }};

        if (srcMappings.size() != 1 || dstMappings.size() != 1) [[unlikely]]
            HandleSplitCopy(srcMappings, dstMappings, srcSize, dstLayerStride, copyFunc);
        else [[likely]]
            copyFunc(srcMappings.front().data(), dstMappings.front().data());
    }

    void MaxwellDma::ReleaseSemaphore() {
        if (registers.launchDma->reductionEnable) [[unlikely]]
            LOGW("Semaphore reduction is unimplemented!");

        u64 address{registers.semaphore->address};
        u64 payload{registers.semaphore->payload};
        switch (registers.launchDma->semaphoreType) {
            case Registers::LaunchDma::SemaphoreType::ReleaseOneWordSemaphore:
                channelCtx.asCtx->gmmu.Write(address, payload);
                LOGD("address: 0x{:X} payload: {}", address, payload);
                break;
            case Registers::LaunchDma::SemaphoreType::ReleaseFourWordSemaphore: {
                // Write timestamp first to ensure correct ordering
                u64 timestamp{GetGpuTimeTicks()};
                channelCtx.asCtx->gmmu.Write(address + 8, timestamp);
                channelCtx.asCtx->gmmu.Write(address, payload);
                LOGD("address: 0x{:X} payload: {} timestamp: {}", address, payload, timestamp);
                break;
            }
            default:
                break;
        }
    }

    void MaxwellDma::CallMethodBatchNonInc(u32 method, span<u32> arguments) {
        for (u32 argument : arguments)
            HandleMethod(method, argument);
    }
}
