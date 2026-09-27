// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <common/circular_queue.h>
#include "syncpoint.h"
#include "frame_queue.h"
#include "classes/class.h"
#include "classes/host1x.h"
#include "classes/nvdec.h"
#include "classes/vic.h"
#include "tegra_host_interface.h"

namespace skyline::soc::host1x {
    /**
     * @brief Represents the command FIFO block of the Host1x controller, with one per each channel allowing them to run asynchronously
     */
    class ChannelCommandFifo {
      private:
        struct QueueEntry {
            enum class Type : u8 {
                Gather,
                CloseStream,
            };

            Type type{};
            span<u32> gather{};
            u64 streamId{};
        };

        const DeviceState &state;

        static constexpr size_t GatherQueueSize{0x1000};
        CircularQueue<QueueEntry> gatherQueue;
        std::thread thread;
        std::mutex threadStartMutex;

        Host1xClass host1XClass;
        TegraHostInterface<NvDecClass> nvDecClass;
        TegraHostInterface<VicClass> vicClass;

        void Send(ClassId targetClass, u32 method, u32 argument, u64 streamId);

        void Process(span<u32> gather, u64 streamId);

        void Run();

      public:
        ChannelCommandFifo(const DeviceState &state, SyncpointSet &syncpoints, FrameQueue &frameQueue);

        ~ChannelCommandFifo();

        void Start();

        /**
         * @brief Pushes one gather together with the nvhost channel identity that submitted it
         */
        void Push(span<u32> gather, u64 streamId);

        /**
         * @brief Queues stream destruction after all previously submitted gathers for that stream
         */
        void CloseStream(u64 streamId);
    };
}
