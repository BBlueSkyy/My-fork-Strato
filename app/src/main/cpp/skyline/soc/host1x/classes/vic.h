// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include "vic/config.h"

namespace skyline::soc::host1x {
    class FrameQueue;

    namespace vic {
        constexpr static size_t RegisterCount{0x446}; //!< The number of 32-bit registers in the VIC method space

        /**
         * @brief The VIC register file, method indices map directly to 32-bit slots
         */
        #pragma pack(push, 1)
        union Registers {
            std::array<u32, RegisterCount> raw;

            struct {
                std::array<u32, 0xC0> _pad0_;
                u32 execute; // 0x300
                std::array<u32, 0x3F> _pad1_;
                std::array<std::array<PlaneOffsets, 8>, 8> surfaces; // 0x400
                u32 pictureIndex; // 0x700
                u32 controlParams; // 0x704
                RegisterOffset configStructOffset; // 0x708
                RegisterOffset filterStructOffset; // 0x70C
                RegisterOffset paletteOffset; // 0x710
                RegisterOffset histOffset; // 0x714
                u32 contextId; // 0x718
                u32 fceUcodeSize; // 0x71C
                PlaneOffsets outputSurface; // 0x720
                RegisterOffset fceUcodeOffset; // 0x72C
            };
        };
        #pragma pack(pop)
        static_assert(offsetof(Registers, execute) == 0x300);
        static_assert(offsetof(Registers, surfaces) == 0x400);
        static_assert(offsetof(Registers, controlParams) == 0x704);
        static_assert(offsetof(Registers, outputSurface) == 0x720);
        static_assert(sizeof(Registers) == RegisterCount * sizeof(u32));
    }

    /**
     * @brief The VIC Host1x class implements hardware accelerated image operations, primarily consuming NVDEC output frames and compositing them into guest-visible surfaces
     * @url https://github.com/NVIDIA/open-gpu-doc (clb0b6 video compositor class)
     */
    class VicClass {
      private:
        const DeviceState &state;
        FrameQueue &frameQueue; //!< Queue of decoded NVDEC frames to consume for surface conversion
        std::function<void()> opDoneCallback;
        vic::Registers registers{};

        /**
         * @brief Performs the composition operation described by the current register state, converting a decoded frame into the guest output surface
         * @note Any errors are logged rather than thrown so a malformed operation can never take down the FIFO thread
         */
        void Execute();

      public:
        VicClass(const DeviceState &state, FrameQueue &frameQueue, std::function<void()> opDoneCallback);

        void CallMethod(u32 method, u32 argument);
    };
}
