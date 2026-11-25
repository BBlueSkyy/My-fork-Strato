// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "channel.h"
#include "gmmu.h"
#include <gpu.h>

namespace skyline::soc::gm20b {
    void ChannelContext::Lock() {
        globalChannelLock.lock();
        try {
            textureMappingLock = std::unique_lock{asCtx->textureMappings->mutex};
            const auto generation{asCtx->textureMappings->Generation()};
            if (generation != mappingGeneration) {
                // Between batches there are no unsubmitted nodes. Existing flush
                // callbacks invalidate TICs/descriptors before any new lookup.
                executor.Submit();
                mappingGeneration = generation;
            }
            executor.LockPreserve();
        } catch (...) {
            if (textureMappingLock.owns_lock()) textureMappingLock.unlock();
            globalChannelLock.unlock();
            throw;
        }
    }

    void ChannelContext::Unlock(bool collect) {
        executor.UnlockPreserve();
        if (collect) {
            try { textureManager.CollectGarbage(*asCtx->textureMappings); }
            catch (const std::bad_alloc &) { /* Collection failure preserves cache ownership. */ }
        }
        textureMappingLock.unlock();
        globalChannelLock.unlock();
    }

    void ChannelContext::ReleaseOnError() {
        if (textureMappingLock.owns_lock()) Unlock(false);
    }

    ChannelContext::ChannelContext(const DeviceState &state, std::shared_ptr<AddressSpaceContext> pAsCtx, size_t numEntries)
        : asCtx{std::move(pAsCtx)},
          executor{state},
          maxwell3D{state, *this, macroState},
          fermi2D{state, *this, macroState},
          maxwellDma{state, *this},
          keplerCompute{state, *this},
          inline2Memory{state, *this},
          globalChannelLock{state.gpu->channelLock},
          textureManager{state.gpu->texture},
          gpfifo{state, *this, numEntries} {
        executor.AddFlushCallback([this] {
            channelSequenceNumber++;
        });
    }
}
