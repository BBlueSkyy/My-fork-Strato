// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <soc/host1x/frame_queue.h>
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

        auto streamIt{streamRegisters.find(streamId)};
        if (streamIt == streamRegisters.end()) {
            streamIt = streamRegisters.try_emplace(streamId).first;
            LOGI("XV2-TRACE VIC stream created stream={}", streamId);
        }

        auto &registers{streamIt->second};
        registers.raw[method] = argument;

        if (method == ExecuteMethodId)
            Execute(registers);
    }

    void VicClass::Execute(vic::Registers &registers) {
        if (!registers.configStructOffset.raw) {
            LOGW("VIC execute without a config struct");
            return;
        }

        try {
            auto config{state.soc->smmu.Read<vic::ConfigStruct>(registers.configStructOffset.Address())};
            auto &surfaceConfig{config.outputSurfaceConfig};

            // FFmpeg returns decoded frames in presentation order for this software path. The
            // current slot-0 luma IOVA is retained for diagnostics, but must not be used to reorder
            // or discard already-decoded presentation frames.
            u64 inputLumaIova{registers.surfaces[0][0].luma.Address()};
            auto frame{frameQueue.PopPresentationFrame(inputLumaIova)};

            LOGD("VIC execute, input luma: 0x{:X}, frame: {}, format: {}, dimensions: {}x{}, layout: {}, output luma: 0x{:X}",
                 inputLumaIova, frame ? "present" : "missing", static_cast<u32>(surfaceConfig.outPixelFormat),
                 u32{surfaceConfig.outLumaWidth} + 1, u32{surfaceConfig.outLumaHeight} + 1,
                 surfaceConfig.outBlkKind == vic::BlkKind::Pitch ? "pitch" : "block-linear",
                 registers.outputSurface.luma.Address());

            if (!frame) {
                // VIC may run before the matching decode becomes available. Do not overwrite the
                // guest surface with diagnostic pixels or with a frame belonging to another surface.
                LOGD("VIC preserving output surface because decoded frame 0x{:X} is unavailable", inputLumaIova);
                return;
            }

            switch (surfaceConfig.outPixelFormat) {
                case vic::VideoPixelFormat::Y8__V8U8_N420:
                case vic::VideoPixelFormat::Y8__U8V8_N420:
                    vic::WriteNv12Surface(state, config, registers.outputSurface, frame.get());
                    break;
                case vic::VideoPixelFormat::A8B8G8R8:
                case vic::VideoPixelFormat::X8B8G8R8:
                case vic::VideoPixelFormat::R8G8B8A8:
                case vic::VideoPixelFormat::A8R8G8B8:
                case vic::VideoPixelFormat::X8R8G8B8:
                    vic::WriteRgbaSurface(state, config, registers.outputSurface, frame.get());
                    break;
                default:
                    LOGW("Unimplemented VIC output pixel format: {}", static_cast<u32>(surfaceConfig.outPixelFormat));
                    break;
            }
        } catch (const std::exception &e) {
            LOGE("VIC execute failed: {}", e.what());
        }
    }

    void VicClass::CloseStream(u64 streamId) {
        streamRegisters.erase(streamId);
        LOGI("XV2-TRACE VIC stream destroyed stream={}", streamId);
    }
}
