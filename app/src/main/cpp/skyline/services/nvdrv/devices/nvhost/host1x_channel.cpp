// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc.h>
#include <services/nvdrv/devices/deserialisation/deserialisation.h>
#include "host1x_channel.h"

namespace skyline::service::nvdrv::device::nvhost {
    /**
     * @return If the Host1x engine classes for this channel are implemented, gating whether submits are pushed to the emulated engines or faked on the CPU
     */
    static constexpr bool IsChannelImplemented(core::ChannelType channelType) {
        return channelType == core::ChannelType::NvDec || channelType == core::ChannelType::VIC;
    }

    Host1xChannel::Host1xChannel(const DeviceState &state,
                                 Driver &driver,
                                 Core &core,
                                 const SessionContext &ctx,
                                 core::ChannelType channelType)
        : NvDevice(state, driver, core, ctx),
          channelType(channelType) {
        state.soc->host1x.channels[static_cast<size_t>(channelType)].Start();
    }

    PosixResult Host1xChannel::SetNvmapFd(In<FileDescriptor> fd) {
        LOGD("fd: {}", fd);
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::Submit(span<SubmitCmdBuf> cmdBufs,
                                      span<SubmitReloc> relocs, span<u32> relocShifts,
                                      span<SubmitSyncpointIncr> syncpointIncrs, span<u32> fenceThresholds) {
        LOGD("numCmdBufs: {}, numRelocs: {}, numSyncpointIncrs: {}, numFenceThresholds: {}",
                            cmdBufs.size(), relocs.size(), syncpointIncrs.size(), fenceThresholds.size());

        if (fenceThresholds.size() > syncpointIncrs.size())
            return PosixResult::InvalidArgument;

        if (relocs.size() != relocShifts.size())
            return PosixResult::InvalidArgument;

        std::scoped_lock lock(channelMutex);

        // Apply relocations: each one patches the pinned SMMU address (IOVA) of `pinMem` (offset by `pinOffset`
        // and shifted by the corresponding entry in `relocShifts`) into `patchMem` at `patchOffset`, so the
        // command buffer contains a valid hardware-visible address before being submitted
        for (size_t i{}; i < relocs.size(); i++) {
            const auto &reloc{relocs[i]};

            auto patchHandle{core.nvMap.GetHandle(reloc.patchMem)};
            if (!patchHandle)
                throw exception("Invalid patch handle passed for a relocation!");

            auto pinHandle{core.nvMap.GetHandle(reloc.pinMem)};
            if (!pinHandle)
                throw exception("Invalid pin handle passed for a relocation!");

            u32 pinAddress{pinHandle->pinVirtAddress};
            if (!pinAddress)
                throw exception("Relocation target handle has not been pinned!");

            u32 word{static_cast<u32>((pinAddress + reloc.pinOffset) >> relocShifts[i])};

            u64 patchAddress{patchHandle->address + reloc.patchOffset};
            *reinterpret_cast<u32 *>(patchAddress) = word;
        }

        bool channelImplemented{IsChannelImplemented(channelType)};

        for (size_t i{}; i < syncpointIncrs.size(); i++) {
            const auto &incr{syncpointIncrs[i]};

            u32 max{core.syncpointManager.IncrementSyncpointMaxExt(incr.syncpointId, incr.numIncrs)};

            if (!channelImplemented) {
                // Increment syncpoints on the CPU for channels whose engines aren't implemented, the fence would never signal otherwise
                for (size_t j{}; j < incr.numIncrs; j++)
                    state.soc->host1x.syncpoints[incr.syncpointId].Increment();
            }

            if (i < fenceThresholds.size())
                fenceThresholds[i] = max;
        }

        for (const auto &cmdBuf : cmdBufs) {
            auto handleDesc{core.nvMap.GetHandle(cmdBuf.mem)};
            if (!handleDesc)
                throw exception("Invalid handle passed for a command buffer!");

            u64 gatherAddress{handleDesc->address + cmdBuf.offset};
            LOGD("Submit gather, CPU address: 0x{:X}, words: 0x{:X}", gatherAddress, cmdBuf.words);

            span gather(reinterpret_cast<u32 *>(gatherAddress), cmdBuf.words);
            // Note: The gather aliases guest memory rather than copying it, correctly synchronised guests won't reuse the cmdbuf before its fence signals
            if (channelImplemented)
                state.soc->host1x.channels[static_cast<size_t>(channelType)].Push(gather);
        }

        return PosixResult::Success;
    }

    PosixResult Host1xChannel::GetSyncpoint(In<u32> channelSyncpointIdx, Out<u32> syncpointId) {
        LOGD("channelSyncpointIdx: {}", channelSyncpointIdx);

        if (channelSyncpointIdx > 0)
            throw exception("Multiple channel syncpoints are unimplemented!");

        u32 id{core::SyncpointManager::ChannelSyncpoints[static_cast<u32>(channelType)]};
        if (!id)
            throw exception("Requested syncpoint for a channel with none specified!");

        LOGD("syncpointId: {}", id);
        syncpointId = id;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::GetWaitBase(In<core::ChannelType> pChannelType, Out<u32> waitBase) {
        LOGD("channelType: {}", static_cast<u32>(pChannelType));
        waitBase = 0;
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::SetSubmitTimeout(In<u32> timeout) {
        LOGD("timeout: {}", timeout);
        return PosixResult::Success;
    }

    PosixResult Host1xChannel::MapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            bufferHandle.address = core.nvMap.PinHandle(bufferHandle.handle);
            LOGD("handle: {}, address: 0x{:X}", bufferHandle.handle, bufferHandle.address);
        }

        return PosixResult::Success;
    }

    PosixResult Host1xChannel::UnmapBuffer(u8 compressed, span<BufferHandle> handles) {
        LOGD("compressed: {}", compressed);

        for (auto &bufferHandle : handles) {
            core.nvMap.UnpinHandle(bufferHandle.handle);
            LOGD("handle: {}", bufferHandle.handle);
        }

        return PosixResult::Success;
    }

// @fmt:off
#include <services/nvdrv/devices/deserialisation/macro_def.inc>
    static constexpr u32 Host1xChannelMagic{0x00};
    static constexpr u32 GpuChannelMagic{0x48}; //!< Used for SetNvmapFd which is needed in both GPU and host1x channels

    VARIABLE_IOCTL_HANDLER_FUNC(Host1xChannel,
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(GpuChannelMagic),    FUNC(0x1),
                        SetNvmapFd,       ARGS(In<FileDescriptor>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x2),
                        GetSyncpoint,     ARGS(In<u32>, Out<u32>))
        IOCTL_CASE_ARGS(INOUT, SIZE(0x8), MAGIC(Host1xChannelMagic), FUNC(0x3),
                        GetWaitBase,      ARGS(In<core::ChannelType>, Out<u32>))
        IOCTL_CASE_ARGS(IN,    SIZE(0x4), MAGIC(Host1xChannelMagic), FUNC(0x7),
                        SetSubmitTimeout, ARGS(In<u32>))
    ,
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x1),
                                 Submit,      ARGS(Save<u32, 0>, Save<u32, 1>, Save<u32, 2>, Save<u32, 3>,
                                                   SlotSizeSpan<SubmitCmdBuf, 0>,
                                                   SlotSizeSpan<SubmitReloc, 1>, SlotSizeSpan<u32, 1>,
                                                   SlotSizeSpan<SubmitSyncpointIncr, 2>, SlotSizeSpan<u32, 3>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0x9),
                                 MapBuffer,   ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
        VARIABLE_IOCTL_CASE_ARGS(INOUT, MAGIC(Host1xChannelMagic), FUNC(0xA),
                                 UnmapBuffer, ARGS(Save<u32, 0>, Pad<u32>, In<u8>, Pad<u8, 3>, SlotSizeSpan<BufferHandle, 0>))
    )
#include <services/nvdrv/devices/deserialisation/macro_undef.inc>
// @fmt:on
}
