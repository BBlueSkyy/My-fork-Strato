// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <soc/host1x/frame_queue.h>
#include <soc/host1x/video_diag.h>
#include "vic/surface_writer.h"
#include "vic.h"

namespace skyline::soc::host1x {
    VicClass::VicClass(const DeviceState &state, FrameQueue &frameQueue, std::function<void()> opDoneCallback)
        : state(state),
          frameQueue(frameQueue),
          opDoneCallback(std::move(opDoneCallback)) {}

    void VicClass::CallMethod(u32 method, u32 argument, u64 streamId) {
        constexpr u32 ExecuteMethodId{offsetof(vic::Registers, execute) / sizeof(u32)};

        if (method >= vic::RegisterCount) {
            LOGW("VIC method out of range: 0x{:X} argument: 0x{:X}", method, argument);
            return;
        }

        auto &registers{streamRegisters[streamId]};
        registers.raw[method] = argument;

        if (method == ExecuteMethodId)
            Execute(registers, streamId);
    }

    void VicClass::Execute(vic::Registers &registers, u64 streamId) {
        static std::atomic<u64> executeCount{0};
        const u64 sequence{++executeCount};
        const bool trace{video_diag::Sample(sequence)};
        if (!registers.configStructOffset.raw) {
            if (trace)
                LOGI("[VideoDiag] vic-no-config seq={} stream={}", sequence, streamId);
            LOGW("VIC execute without a config struct");
            return;
        }

        try {
            auto config{state.soc->smmu.Read<vic::ConfigStruct>(registers.configStructOffset.Address())};
            auto &surfaceConfig{config.outputSurfaceConfig};
            u32 enabledSlots{};
            for (size_t slot{}; slot < config.slotStructs.size(); ++slot)
                if (config.slotStructs[slot].config.slotEnable)
                    enabledSlots |= (1U << slot);

            // FFmpeg returns decoded frames in presentation order for this software path. The
            // current slot-0 luma IOVA is retained for diagnostics, but must not be used to reorder
            // or discard already-decoded presentation frames.
            u64 inputLumaIova{registers.surfaces[0][0].luma.Address()};
            auto frame{frameQueue.PopPresentationFrame(inputLumaIova)};
            if (trace)
                LOGI("[VideoDiag] vic-execute seq={} stream={} inputLuma=0x{:X} slot0Format={} slotMask=0x{:X} frame={} format={} size={}x{} blkKind={} blkHeight={} output=0x{:X}",
                     sequence, streamId, inputLumaIova,
                     static_cast<u32>(config.slotStructs[0].surfaceConfig.slotPixelFormat), enabledSlots,
                     static_cast<bool>(frame), static_cast<u32>(surfaceConfig.outPixelFormat),
                     u32{surfaceConfig.outLumaWidth} + 1, u32{surfaceConfig.outLumaHeight} + 1,
                     static_cast<u32>(surfaceConfig.outBlkKind), static_cast<u32>(surfaceConfig.outBlkHeight),
                     registers.outputSurface.luma.Address());

            LOGD("VIC execute, input luma: 0x{:X}, frame: {}, format: {}, dimensions: {}x{}, layout: {}, output luma: 0x{:X}",
                 inputLumaIova, frame ? "present" : "missing", static_cast<u32>(surfaceConfig.outPixelFormat),
                 u32{surfaceConfig.outLumaWidth} + 1, u32{surfaceConfig.outLumaHeight} + 1,
                 surfaceConfig.outBlkKind == vic::BlkKind::Pitch ? "pitch" : "block-linear",
                 registers.outputSurface.luma.Address());

            if (!frame) {
                // VIC may run before the matching decode becomes available. Do not overwrite the
                // guest surface with diagnostic pixels or with a frame belonging to another surface.
                LOGD("VIC preserving output surface because decoded frame 0x{:X} is unavailable", inputLumaIova);
                if (trace)
                    LOGI("[VideoDiag] vic-frame-missing seq={} stream={} input=0x{:X}", sequence, streamId, inputLumaIova);
                return;
            }

            bool dispatched{};
            switch (surfaceConfig.outPixelFormat) {
                case vic::VideoPixelFormat::Y8__V8U8_N420:
                case vic::VideoPixelFormat::Y8__U8V8_N420:
                    vic::WriteNv12Surface(state, config, registers.outputSurface, frame.get(), trace);
                    dispatched = true;
                    break;
                case vic::VideoPixelFormat::A8B8G8R8:
                case vic::VideoPixelFormat::X8B8G8R8:
                case vic::VideoPixelFormat::R8G8B8A8:
                case vic::VideoPixelFormat::A8R8G8B8:
                case vic::VideoPixelFormat::X8R8G8B8:
                    vic::WriteRgbaSurface(state, config, registers.outputSurface, frame.get(), trace);
                    dispatched = true;
                    break;
                default:
                    LOGW("Unimplemented VIC output pixel format: {}", static_cast<u32>(surfaceConfig.outPixelFormat));
                    break;
            }
            if (trace)
                LOGI("[VideoDiag] vic-dispatch seq={} stream={} dispatched={} format={} output=0x{:X}",
                     sequence, streamId, dispatched, static_cast<u32>(surfaceConfig.outPixelFormat),
                     registers.outputSurface.luma.Address());
        } catch (const std::exception &e) {
            LOGE("VIC execute failed: {}", e.what());
        }
    }

    void VicClass::CloseStream(u64 streamId) {
        streamRegisters.erase(streamId);
    }
}
