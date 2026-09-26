// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <unistd.h>
#include <logger/logger.h>
#include <gpu/interconnect/command_executor.h>
#include "macro/macro_state.h"
#include "engines/engine.h"
#include "engines/maxwell_3d.h"
#include "engines/fermi_2d.h"
#include "engines/maxwell_dma.h"
#include "engines/kepler_compute.h"
#include "engines/inline2memory.h"
#include "gpfifo.h"

namespace skyline::soc::gm20b {
    struct AddressSpaceContext;

    /**
     * @brief The GPU block in the X1, it contains all GPU engines required for accelerating graphics operations
     * @note We omit parts of components related to external access such as the grhost, all accesses to the external components are done directly
     */
    struct ChannelContext {
        std::shared_ptr<AddressSpaceContext> asCtx;
        gpu::interconnect::CommandExecutor executor;
        MacroState macroState;
        engine::maxwell3d::Maxwell3D maxwell3D;
        engine::fermi2d::Fermi2D fermi2D;
        engine::MaxwellDma maxwellDma;
        engine::KeplerCompute keplerCompute;
        engine::Inline2Memory inline2Memory;
        ChannelGpfifo gpfifo;
        std::mutex &globalChannelLock;
        size_t channelSequenceNumber{};

        ChannelContext(const DeviceState &state, std::shared_ptr<AddressSpaceContext> asCtx, size_t numEntries);

        ~ChannelContext();

        void Lock() {
            LOGI("GRID-LOCK ChannelContext::Lock global-begin tid={} this={}",
                 gettid(), static_cast<const void *>(this));
            globalChannelLock.lock();
            LOGI("GRID-LOCK ChannelContext::Lock global-acquired tid={} this={}",
                 gettid(), static_cast<const void *>(this));

            LOGI("GRID-LOCK ChannelContext::Lock executor-begin tid={} this={}",
                 gettid(), static_cast<const void *>(this));
            executor.LockPreserve();
            LOGI("GRID-LOCK ChannelContext::Lock executor-acquired tid={} this={}",
                 gettid(), static_cast<const void *>(this));
        }

        void Unlock() {
            LOGI("GRID-LOCK ChannelContext::Unlock executor-begin tid={} this={}",
                 gettid(), static_cast<const void *>(this));
            executor.UnlockPreserve();
            LOGI("GRID-LOCK ChannelContext::Unlock executor-end tid={} this={}",
                 gettid(), static_cast<const void *>(this));

            LOGI("GRID-LOCK ChannelContext::Unlock global-begin tid={} this={}",
                 gettid(), static_cast<const void *>(this));
            globalChannelLock.unlock();
            LOGI("GRID-LOCK ChannelContext::Unlock global-end tid={} this={}",
                 gettid(), static_cast<const void *>(this));
        }
    };
}
