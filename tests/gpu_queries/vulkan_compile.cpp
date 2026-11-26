// SPDX-License-Identifier: MPL-2.0
// Compile the unchanged production query classes and helper bodies with the
// repository's pinned Vulkan-Hpp. Only non-Vulkan engine interfaces are declared.
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using ContextTag = u64;
namespace skyline {
template <class T> struct span {
    explicit span(T &);
    span(T *, size_t);
    template <class U> span<U> cast() const;
};
namespace vfs {
struct Backing;
struct FileSystem {
    std::shared_ptr<Backing> OpenFile(const char *);
};
} // namespace vfs
namespace soc::gm20b {
using IOVA = u64;
}
namespace gpu {
struct GPU;
struct exception {
    template <class... Args> exception(const char *, Args &&...);
};
template <class T> struct ContextLock {
    ContextLock(ContextTag, T &);
    bool IsFirstUsage() const;
};
struct FenceCycle {
    bool Poll(bool);
};
namespace memory {
struct Buffer {
    vk::Buffer vkBuffer;
    size_t size_bytes() const;
};
} // namespace memory
struct DescriptorAllocator {
    struct ActiveDescriptorSet {
        ActiveDescriptorSet(ActiveDescriptorSet &&) noexcept;
        vk::DescriptorSet &operator*() const;
    };
    ActiveDescriptorSet AllocateSet(vk::DescriptorSetLayout);
};
struct BufferBinding {
    vk::Buffer buffer{};
    vk::DeviceSize offset{}, size{};
    explicit operator bool() const;
};
struct Buffer {
    std::shared_ptr<Buffer> shared_from_this();
    bool IsCurrentExecutionGpuDirty() const;
    u64 GetWriteGeneration() const;
    void BlockSequencedCpuBackingWrites();
    void MarkGpuDirty(int &);
};
struct BufferView {
    BufferBinding GetBinding(GPU &) const;
    Buffer *GetBuffer() const;
    vk::DeviceSize GetOffset() const;
    vk::DeviceSize size{};
    operator bool();
#include "buffer_read.inc"
};
namespace interconnect {
struct InterconnectContext;
}
struct CachedMappedBufferView {
    BufferView view;
    void Update(interconnect::InterconnectContext &, u64, u32);
    BufferView &operator*();
    BufferView *operator->();
    void PurgeCaches();
};
#include "helpers_header.inc"
struct GPU {
    vk::raii::Device vkDevice;
    struct {
        bool supportsOcclusionQueryPrecise;
        bool supportsConditionalRendering;
    } traits;
    struct {
        memory::Buffer AllocateBuffer(size_t);
    } memory;
    DescriptorAllocator descriptor;
    struct {
        QueryResolveHelperShader queryResolveHelperShader;
        ConditionalCompareHelperShader conditionalCompareHelperShader;
    } helperShaders;
    struct {
        template <class Bytes> BufferBinding Push(const std::shared_ptr<FenceCycle> &, Bytes);
    } megaBufferAllocator;
};
static vk::raii::ShaderModule CreateShaderModule(GPU &, vfs::Backing &);
#include "helpers_source.inc"
namespace interconnect {
struct HostAllocator {
    template <class T> T *EmplaceUntracked(T);
};
struct CommandExecutor {
    using CommandFunction = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)>;
    using SubpassFunction =
        std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)>;
    struct SubpassHooks {
        SubpassFunction before;
        CommandFunction beforeRenderPassEnd, afterRenderPass;
    };
    ContextTag executionTag;
    ContextTag tag;
    std::shared_ptr<FenceCycle> cycle;
    HostAllocator *allocator;
    int usageTracker;
    void AttachDependency(const std::shared_ptr<void> &);
    void AttachBuffer(BufferView &);
    void BreakRenderPass();
    void AddCommand(CommandFunction &&);
    void InsertPreExecuteCommand(CommandFunction &&);
    void InsertPostRpCommand(CommandFunction &&);
    void AddPostRpCommand(CommandFunction &&);
    void AddOutsideRpCommand(CommandFunction &&);
    void Submit();
};
struct InterconnectContext {
    GPU &gpu;
    CommandExecutor &executor;
};
} // namespace interconnect
} // namespace gpu
} // namespace skyline
#include "queries_header.inc"
#include "queries_source.inc"

#include "conditional_header.inc"
#include "conditional_source.inc"
