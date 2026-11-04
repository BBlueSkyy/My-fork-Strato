// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/gmmu.h>
#include <soc/gm20b/channel.h>
#include "maxwell_dma.h"

namespace skyline::gpu::interconnect {
    MaxwellDma::MaxwellDma(GPU &gpu, soc::gm20b::ChannelContext &channelCtx)
        : gpu{gpu},
          channelCtx{channelCtx},
          executor{channelCtx.executor} {}

    void MaxwellDma::Copy(soc::gm20b::IOVA dst, soc::gm20b::IOVA src, size_t size) {
        executor.RunWithTextureBackingRetry([&] {
            auto &gmmu{channelCtx.asCtx->gmmu};
            const auto epoch{gmmu.ReadMappingEpoch()};
            executor.StartTextureMapping(epoch);
            auto srcMappings{gmmu.TranslateRange(src, size)};
            auto dstMappings{gmmu.TranslateRange(dst, size)};
            if (srcMappings.size() != 1 || dstMappings.size() != 1) {
                // Legacy fragmented/sparse copy retranslates under the GMMU's
                // table lock. No cached physical spans escape this attempt.
                gmmu.Copy(dst, src, size);
                return;
            }
            auto attach{[this](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                executor.AttachLockedBuffer(buffer, std::move(lock));
            }};
            auto srcView{gpu.buffer.FindOrCreate(srcMappings.front(), executor.tag, attach)};
            auto originalSource{srcView.GetBuffer()->shared_from_this()};
            ContextLock originalLock{executor.tag, *originalSource};
            auto dstView{gpu.buffer.FindOrCreate(dstMappings.front(), executor.tag, attach)};
            // Retain and lock concrete objects, never mutable delegates. The
            // second lookup can replace the first object's delegate target.
            auto source{srcView.GetBuffer()->shared_from_this()};
            auto destination{dstView.GetBuffer()->shared_from_this()};
            source->RequireRegionCoherence(executor.tag);
            destination->RequireRegionCoherence(executor.tag);
            ContextLock sourceLock{executor.tag, *source};
            ContextLock destinationLock{executor.tag, *destination};
            if (destination->TryMappedCopy(dstView.GetOffset(), source.get(), srcView.GetOffset(), size, executor.usageTracker, epoch)) return;
            executor.AttachLockedBuffer(originalSource, std::move(originalLock));
            executor.AttachLockedBuffer(source, std::move(sourceLock));
            executor.AttachLockedBuffer(destination, std::move(destinationLock));
            executor.AttachBuffer(srcView);
            executor.AttachBuffer(dstView);
            source->SynchronizeHost();
            source->BlockAllCpuBackingWrites();
            destination->PrepareGpuWrite(executor.usageTracker);
            std::shared_ptr<memory::StagingBuffer> intermediate;
            if (source == destination && srcView.GetOffset() < dstView.GetOffset() + size && dstView.GetOffset() < srcView.GetOffset() + size) {
                intermediate = gpu.memory.AllocateStagingBuffer(size);
                executor.AttachDependency(intermediate);
            }
            executor.AddOutsideRpCommand([srcView, dstView, intermediate, size](vk::raii::CommandBuffer &commands, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                auto barrier{[&] {
                    commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, vk::MemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite
                    }, {}, {});
                }};
                barrier();
                auto srcBinding{srcView.GetBinding(gpu)};
                auto dstBinding{dstView.GetBinding(gpu)};
                if (intermediate) {
                    commands.copyBuffer(srcBinding.buffer, intermediate->vkBuffer, vk::BufferCopy{.srcOffset = srcBinding.offset, .dstOffset = 0, .size = size});
                    barrier();
                    commands.copyBuffer(intermediate->vkBuffer, dstBinding.buffer, vk::BufferCopy{.srcOffset = 0, .dstOffset = dstBinding.offset, .size = size});
                } else {
                    commands.copyBuffer(srcBinding.buffer, dstBinding.buffer, vk::BufferCopy{.srcOffset = srcBinding.offset, .dstOffset = dstBinding.offset, .size = size});
                }
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                }, {}, {});
            }, {}, span<BufferView>{dstView});
        });
    }

    void MaxwellDma::Clear(soc::gm20b::IOVA dst, size_t size, u32 value) {
        executor.RunWithTextureBackingRetry([&] {
            auto &gmmu{channelCtx.asCtx->gmmu};
            executor.StartTextureMapping(gmmu.ReadMappingEpoch());
            const auto mappings{gmmu.TranslateRange(dst, size)};
            for (auto mapping : mappings)
                if (!util::IsAligned(mapping.size(), 4) || !util::IsAligned(reinterpret_cast<uintptr_t>(mapping.data()), 4))
                    throw exception("Cleared buffer range is not aligned to 4 bytes!");
            boost::container::small_vector<BufferView, 4> views;
            for (auto mapping : mappings)
                views.push_back(gpu.buffer.FindOrCreate(mapping, executor.tag, [this](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                    executor.AttachLockedBuffer(buffer, std::move(lock));
                }));
            for (auto &view : views) {
                executor.AttachBuffer(view);
                view.GetBuffer()->PrepareGpuWrite(executor.usageTracker);
            }
            executor.AddOutsideRpCommand([views, value](vk::raii::CommandBuffer &commands, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                for (auto view : views) {
                    commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, vk::MemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite
                    }, {}, {});
                    auto binding{view.GetBinding(gpu)};
                    commands.fillBuffer(binding.buffer, binding.offset, binding.size, value);
                }
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                }, {}, {});
            }, {}, span<BufferView>{views.data(), views.size()});
        });
    }
}
