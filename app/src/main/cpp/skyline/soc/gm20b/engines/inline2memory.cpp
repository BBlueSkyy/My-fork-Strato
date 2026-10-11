// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/texture/layout.h>
#include <gpu/diagnostic_copy_tracker.h>
#include <soc/gm20b/channel.h>
#include "inline2memory.h"

namespace skyline::soc::gm20b::engine {
    Inline2MemoryBackend::Inline2MemoryBackend(const DeviceState &state, ChannelContext &channelCtx)
        : interconnect{*state.gpu, channelCtx},
          channelCtx{channelCtx} {}

    void Inline2MemoryBackend::LaunchDma(Inline2MemoryBackend::RegisterState &state) {
        writeOffset = 0;
        size_t targetSizeWords{(state.lineCount * util::AlignUp(state.lineLengthIn, 4)) / 4};
        buffer.resize(targetSizeWords);
    }

    void Inline2MemoryBackend::CompleteDma(Inline2MemoryBackend::RegisterState &state) {
        if (state.launchDma.completion == RegisterState::DmaCompletionType::ReleaseSemaphore)
            throw exception("Semaphore release on I2M completion is not supported!");

        LOGD("range: 0x{:X} -> 0x{:X}", u64{state.offsetOut}, u64{state.offsetOut} + buffer.size() * 0x4);

        // Include inline-to-memory uploads: the ordinary DMA and Fermi2D
        // diagnostics do not observe this GPU engine.
        if (gpu::diagnostic::LoggedInlineMatches().load(std::memory_order_relaxed) < 128) {
            try {
                const auto destinations{channelCtx.asCtx->gmmu.TranslateRange(state.offsetOut, 1)};
                if (!destinations.empty()) {
                    const void *destination{destinations.front().data()};
                    if (const auto region{gpu::diagnostic::FindPresentationSurface(destination)}) {
                        const auto index{gpu::diagnostic::LoggedInlineMatches().fetch_add(1, std::memory_order_relaxed)};
                        if (index < 128) {
                            LOGI("MINIRD_I2M_TO_PRESENT event={} dst_iova=0x{:X} dst_host=0x{:X} "
                                 "present_base=0x{:X} present_offset=0x{:X} layout={} "
                                 "line_bytes={} lines={} pitch={} dst_surface={}x{}x{} "
                                 "block_height={} origin={},{}",
                                 index, u64{state.offsetOut}, reinterpret_cast<uintptr_t>(destination),
                                 region->base, reinterpret_cast<uintptr_t>(destination) - region->base,
                                 static_cast<u32>(state.launchDma.layout),
                                 state.lineLengthIn, state.lineCount, state.pitchOut,
                                 state.dstWidth, state.dstHeight, state.dstDepth,
                                 state.dstBlockSize.Height(),
                                 state.originBytesX, state.originSamplesY);
                        }
                    }
                }
            } catch (const std::exception &) {
                // Diagnostic translation must not affect I2M error behavior.
            }
        }
        if (state.launchDma.layout == RegisterState::DmaDstMemoryLayout::Pitch) {
            channelCtx.channelSequenceNumber++;

            auto srcBuffer{span{buffer}.cast<u8>()};
            for (u32 line{}, pitchOffset{}; line < state.lineCount; ++line, pitchOffset += state.pitchOut)
                interconnect.Upload(u64{state.offsetOut + pitchOffset}, srcBuffer.subspan(state.lineLengthIn * line, state.lineLengthIn));

        } else {
            channelCtx.executor.Submit();

            gpu::texture::Dimensions srcDimensions{state.lineLengthIn, state.lineCount, state.dstDepth};

            gpu::texture::Dimensions dstDimensions{state.dstWidth, state.dstHeight, state.dstDepth};
            size_t dstSize{GetBlockLinearLayerSize(dstDimensions, 1, 1, 1, 1 << (u8)state.dstBlockSize.height, 1 << (u8)state.dstBlockSize.depth)};

            auto dstMappings{channelCtx.asCtx->gmmu.TranslateRange(state.offsetOut, dstSize)};

            auto inlineCopy{[&](u8 *dst){
                // The I2M engine only supports a formatBpb of 1
                if ((srcDimensions.width != dstDimensions.width) || (srcDimensions.height != dstDimensions.height))
                    gpu::texture::CopyLinearToBlockLinearSubrect(srcDimensions, dstDimensions,
                                                                 1, 1, 1,
                                                                 state.dstBlockSize.Height(), state.dstBlockSize.Depth(),
                                                                 span{buffer}.cast<u8>().data(), dst,
                                                                 state.originBytesX, state.originSamplesY
                    );
                else
                    gpu::texture::CopyLinearToBlockLinear(dstDimensions,
                                                          1, 1, 1,
                                                          state.dstBlockSize.Height(), state.dstBlockSize.Depth(),
                                                          span{buffer}.cast<u8>().data(), dst
                    );
            }};

            if (dstMappings.size() != 1) {
                // We create a temporary buffer to hold the blockLinear texture if mappings are split
                // NOTE: We don't reserve memory here since such copies on this engine are rarely used
                std::vector<u8> tempBuffer(dstSize);

                inlineCopy(tempBuffer.data());

                channelCtx.asCtx->gmmu.Write(state.offsetOut, span(tempBuffer));
            } else {
                inlineCopy(dstMappings.front().data());
            }
        }
    }

    void Inline2MemoryBackend::LoadInlineData(RegisterState &state, u32 value) {
        if (writeOffset >= buffer.size())
            throw exception("Inline data load overflow!");

        buffer[writeOffset++] = value;

        if (writeOffset == buffer.size())
            CompleteDma(state);
    }

    void Inline2MemoryBackend::LoadInlineData(Inline2MemoryBackend::RegisterState &state, span<u32> data) {
        if (writeOffset + data.size() > buffer.size())
            throw exception("Inline data load overflow!");

        span(buffer).subspan(writeOffset).copy_from(data);
        writeOffset += data.size();

        if (writeOffset == buffer.size())
            CompleteDma(state);
    }

    Inline2Memory::Inline2Memory(const DeviceState &state, ChannelContext &channelCtx) : backend{state, channelCtx} {}

    __attribute__((always_inline)) void Inline2Memory::CallMethod(u32 method, u32 argument) {
        LOGV("Called method in I2M: 0x{:X} args: 0x{:X}", method, argument);

        HandleMethod(method, argument);
    }

    void Inline2Memory::HandleMethod(u32 method, u32 argument) {
        registers.raw[method] = argument;

        switch (method) {
            ENGINE_STRUCT_CASE(i2m, launchDma, {
                backend.LaunchDma(*registers.i2m);
            })
            ENGINE_STRUCT_CASE(i2m, loadInlineData, {
                backend.LoadInlineData(*registers.i2m, argument);
            })
            default:
                return;
        }

    }

    void Inline2Memory::CallMethodBatchNonInc(u32 method, span<u32> arguments) {
        switch (method) {
            case ENGINE_STRUCT_OFFSET(i2m, loadInlineData):
                backend.LoadInlineData(*registers.i2m, arguments);
                return;
            default:
                break;
        }

        for (u32 argument : arguments)
            HandleMethod(method, argument);
    }
}
