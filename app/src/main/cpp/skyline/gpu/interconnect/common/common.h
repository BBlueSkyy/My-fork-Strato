// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <source_location>
#include <gpu/diagnostic/re5_descriptor_trace.h>
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
namespace skyline::gpu { class TextureView; }

namespace skyline::gpu::interconnect {
    namespace engine_common = skyline::soc::gm20b::engine;

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
        void Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn = true);

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
                  std::source_location location = std::source_location::current());

        template<typename T>
        T Read(CommandExecutor &executor, size_t srcOffset,
               std::source_location location = std::source_location::current()) {
            T object;
            Read(executor, span<T>{object}.template cast<u8>(), srcOffset, location);
            return object;
        }
    };

    using DynamicBufferBinding = std::variant<BufferBinding, BufferView>;
    using DirtyManager = dirty::Manager<soc::gm20b::engine::EngineMethodsEnd * sizeof(u32), sizeof(u32)>;

    class StateUpdateBuilder;

    // Points at the exact descriptor info filled by StateUpdater during recording.
    // Valid only within the owning executor allocation/cycle; never re-resolve a
    // BufferView here, as that could select a different backing than the draw.
    struct DiagnosticUniformBuffer {
        const vk::DescriptorBufferInfo *info{};
        u32 binding{};
        u32 element{};
        bool immutableMegaBuffer{};
    };

    struct DiagnosticScalarProbe {
        DiagnosticUniformBuffer descriptor;
        u32 byteOffset{};
        u32 index{};
        std::string directory;
        std::string metadata;
        std::string result;
        bool copied{};
    };

    struct DescriptorUpdateInfo {
        span<vk::CopyDescriptorSet> copies; //!< These will be performed before writes
        span<vk::WriteDescriptorSet> writes;
        span<vk::DescriptorBufferInfo> bufferDescs;
        span<DynamicBufferBinding> bufferDescDynamicBindings;
        vk::PipelineLayout pipelineLayout;
        vk::DescriptorSetLayout descriptorSetLayout;
        vk::PipelineBindPoint bindPoint;
        u32 descriptorSetIndex;
        // Diagnostic slot 0 is FS cbuf 12 (explicit cubemap LOD source for F380);
        // slot 1 retains FS cbuf 4 as a control. These are observation-only.
        std::array<DiagnosticUniformBuffer, 2> diagnosticUniformBuffers{};
        span<DiagnosticImageBinding> diagnosticImageBindings{};
    };
}
