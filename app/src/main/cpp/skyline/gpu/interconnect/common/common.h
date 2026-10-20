// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <source_location>
#include <common/dirty_tracking.h>
#include <vulkan/vulkan_raii.hpp>
#include <gpu/buffer.h>
#include <soc/gm20b/engines/engine.h>

namespace skyline::kernel {
    class MemoryManager;
}

namespace skyline::soc::gm20b {
    struct ChannelContext;
}

namespace skyline::gpu::interconnect {
    class CommandExecutor;
}

namespace skyline::gpu::interconnect {
    namespace engine_common = skyline::soc::gm20b::engine;

    // Ask the surrounding draw-preparation retry boundary to submit pending
    // writes before compiling a shader that requires their CPU-visible result.
    // Thrown only for a GPU-dirty constant-buffer read performed mid-draw.
    struct GpuDirtyConstantBufferReadRequest {};

    /**
     * @brief Holds GPU context for an interconnect instance
     */
    struct InterconnectContext {
        soc::gm20b::ChannelContext &channelCtx;
        CommandExecutor &executor;
        GPU &gpu;
        TrapManager &trap;
        kernel::MemoryManager &memory;
    };

    /**
     * @brief Helper around a buffer view that performs caching based on the underlying GPU mappings
     */
    class CachedMappedBufferView {
      private:
        span<u8> blockMapping; //!< The underlying mapping that `view` is a part of
        u64 blockMappingStartAddr; //!< The start GPU address of `blockMapping`
        u64 blockMappingEndAddr; //!< The end GPU address of `blockMapping`

      public:
        BufferView view{}; //!< The buffer view created as a result of a call to `Update()`

        /**
         * @brief Updates `view` based on the supplied GPU mapping
         */
        void Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn = true,
                    std::source_location location = std::source_location::current());

        /**
         * @brief Purges the cached block mapping so the next `Update()` call will perform a full lookup
         */
        void PurgeCaches();

        BufferView &operator*() {
            return view;
        }

        BufferView *operator->() {
            return &view;
        }
    };

    struct ShaderBinary {
        span<u8> binary;
        u32 baseOffset;
    };

    struct ConstantBuffer {
        BufferView view;

        void Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset,
                  std::source_location location = std::source_location::current(),
                  bool retryPendingGpuWrites = false);

        template<typename T>
        T Read(CommandExecutor &executor, size_t srcOffset,
               std::source_location location = std::source_location::current()) {
            T object;
            Read(executor, span<T>{object}.template cast<u8>(), srcOffset, location);
            return object;
        }

        // Only call from an operation guarded by RunWithTextureBackingRetry.
        // Unlike a normal read, attached GPU-dirty data must unwind the draw
        // before submitting, then reconstruct shader/pipeline state on retry.
        template<typename T>
        T ReadForShaderCompile(CommandExecutor &executor, size_t srcOffset,
                               std::source_location location = std::source_location::current()) {
            T object;
            Read(executor, span<T>{object}.template cast<u8>(), srcOffset, location, true);
            return object;
        }
    };

    using DynamicBufferBinding = std::variant<BufferBinding, BufferView>;
    using DirtyManager = dirty::Manager<soc::gm20b::engine::EngineMethodsEnd * sizeof(u32), sizeof(u32)>;

    class StateUpdateBuilder;

    struct DescriptorUpdateInfo {
        span<vk::CopyDescriptorSet> copies; //!< These will be performed before writes
        span<vk::WriteDescriptorSet> writes;
        span<vk::DescriptorBufferInfo> bufferDescs;
        span<DynamicBufferBinding> bufferDescDynamicBindings;
        vk::PipelineLayout pipelineLayout;
        vk::DescriptorSetLayout descriptorSetLayout;
        vk::PipelineBindPoint bindPoint;
        u32 descriptorSetIndex;
    };
}
