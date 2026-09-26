// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <logger/logger.h>
#include "channel.h"

namespace skyline::soc::gm20b {
    ChannelContext::ChannelContext(const DeviceState &state, std::shared_ptr<AddressSpaceContext> pAsCtx, size_t numEntries)
        : asCtx{std::move(pAsCtx)},
          executor{state},
          maxwell3D{state, *this, macroState},
          fermi2D{state, *this, macroState},
          maxwellDma{state, *this},
          keplerCompute{state, *this},
          inline2Memory{state, *this},
          gpfifo{state, *this, numEntries},
          globalChannelLock{state.gpu->channelLock} {
        executor.AddFlushCallback([this] {
            channelSequenceNumber++;
        });
        LOGI("GRID-LIFE ChannelContext ctor this={} gpfifo={} numEntries={}",
             static_cast<const void *>(this), static_cast<const void *>(&gpfifo), numEntries);
    }

    ChannelContext::~ChannelContext() {
        LOGI("GRID-LIFE ChannelContext dtor-body this={} gpfifo={}",
             static_cast<const void *>(this), static_cast<const void *>(&gpfifo));
    }
}
