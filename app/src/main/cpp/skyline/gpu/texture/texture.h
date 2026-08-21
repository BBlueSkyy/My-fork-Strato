// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <range/v3/view.hpp>
#include <range/v3/algorithm.hpp>
#include <common/spin_lock.h>
#include <common/lockable_shared_ptr.h>
#include "common/trap_manager.h"
#include <gpu/tag_allocator.h>
#include <gpu/memory_manager.h>
#include <gpu/usage_tracker.h>

namespace skyline::gpu {
    namespace texture {
        enum class RenderPassUsage : u8 {
            None,
            Sampled,
            RenderTarget
        };

        struct Dimensions {
            u32 width;
            u32 height;
            u32 depth;

            constexpr Dimensions() : width(0), height(0), depth(0) {}

            constexpr Dimensions(u32 width) : width(width), height(1), depth(1) {}

            constexpr Dimensions(u32 width, u32 height) : width(width), height(height), depth(1) {}

            constexpr Dimensions(u32 width, u32 height, u32 depth) : width(width), height(height), depth(depth) {}

            constexpr Dimensions(vk::Extent2D extent) : Dimensions(extent.width, extent.height) {}

            constexpr Dimensions(vk::Extent3D extent) : Dimensions(extent.width, extent.height, extent.depth) {}

            auto operator<=>(const Dimensions &) const = default;

            constexpr operator vk::Extent2D() const {
                return vk::Extent2D{
                    .width = width,
                    .height = height,
                };
            }

            constexpr operator vk::Extent3D() const {
                return vk::Extent3D{
                    .width = width,
                    .height = height,
                    .depth = depth,
                };
            }

            constexpr operator bool() const {
                return width && height && depth;
            }
        };

        /**
         * @note Blocks refers to the atomic unit of a compressed format
         */
        struct FormatBase {
            u8 bpb{};
            vk::Format vkFormat{vk::Format::eUndefined};
            vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
            u16 blockHeight{1};
            u16 blockWidth{1};
            vk::ComponentMapping swizzleMapping{
                .r = vk::ComponentSwizzle::eR,
                .g = vk::ComponentSwizzle::eG,
                .b = vk::ComponentSwizzle::eB,
                .a = vk::ComponentSwizzle::eA
            };
            bool stencilFirst{};

            constexpr bool IsCompressed() const {
                return (blockHeight != 1) || (blockWidth != 1);
            }

            constexpr size_t GetSize(u32 width, u32 height, u32 depth = 1) const {
                return util::DivideCeil<size_t>(width, size_t{blockWidth}) * util::DivideCeil<size_t>(height, size_t{blockHeight}) * bpb * depth;
            }

            constexpr size_t GetSize(Dimensions dimensions) const {
                return GetSize(dimensions.width, dimensions.height, dimensions.depth);
            }

            constexpr bool operator==(const FormatBase &format) const {
                return vkFormat == format.vkFormat;
            }

            constexpr bool operator!=(const FormatBase &format) const {
                return vkFormat != format.vkFormat;
            }

            constexpr operator vk::Format() const {
                return vkFormat;
            }

            constexpr operator bool() const {
                return bpb;
            }

            constexpr bool IsCompatible(const FormatBase &other) const {
                return vkFormat == other.vkFormat
                    || (vkFormat == vk::Format::eD32Sfloat && other.vkFormat == vk::Format::eR32Sfloat)
                    || (!vk::isCompressed(vkFormat) && !vk::isCompressed(other.vkFormat) && componentCount(vkFormat) == componentCount(other.vkFormat) &&
                        ranges::all_of(ranges::views::iota(u8{0}, componentCount(vkFormat)), [this, other](auto i) {
                            return componentBits(vkFormat, i) == componentBits(other.vkFormat, i);
                        })
                        && (vkAspect & other.vkAspect) != vk::ImageAspectFlags{});
            }

            constexpr vk::ImageAspectFlags Aspect(bool first) const {
                if (vkAspect & vk::ImageAspectFlagBits::eDepth && vkAspect & vk::ImageAspectFlagBits::eStencil) {
                    if (first)
                        return stencilFirst ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlagBits::eDepth;
                    else
                        return stencilFirst ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eStencil;
                } else {
                    return vkAspect;
                }
            }
        };

        class Format {
          private:
            const FormatBase *base;

          public:
            constexpr Format(const FormatBase &base) : base(&base) {}

            constexpr Format() : base(nullptr) {}

            constexpr const FormatBase *operator->() const {
                return base;
            }

            constexpr const FormatBase &operator*() const {
                return *base;
            }

            constexpr bool operator==(const Format &format) const {
                return base && format.base ? (*base) == (*format.base) : base == format.base;
            }

            constexpr bool operator!=(const Format &format) const {
                return base && format.base ? (*base) != (*format.base) : base != format.base;
            }

            constexpr operator bool() const {
                return base;
            }
        };

        enum class TileMode {
            Linear,
            Pitch,
            Block,
        };

        struct TileConfig {
            TileMode mode;
            union {
                struct {
                    u8 blockHeight;
                    u8 blockDepth;
                };
                u32 pitch;
            };

            constexpr bool operator==(const TileConfig &other) const {
                if (mode == other.mode) {
                    switch (mode) {
                        case TileMode::Linear:
                            return true;
                        case TileMode::Pitch:
                            return pitch == other.pitch;
                        case TileMode::Block:
                            return blockHeight == other.blockHeight && blockDepth == other.blockDepth;
                    }
                }

                return false;
            }
        };

        struct MipLevelLayout {
            Dimensions dimensions;
            size_t linearSize;
            size_t targetLinearSize;
            size_t blockLinearSize;
            size_t blockHeight, blockDepth;

            constexpr MipLevelLayout(Dimensions dimensions, size_t linearSize, size_t targetLinearSize, size_t blockLinearSize, size_t blockHeight, size_t blockDepth) : dimensions{dimensions}, linearSize{linearSize}, targetLinearSize{targetLinearSize}, blockLinearSize{blockLinearSize}, blockHeight{blockHeight}, blockDepth{blockDepth} {}
        };
    }

    class Texture;
    class PresentationEngine;

    struct GuestTexture {
        using Mappings = boost::container::small_vector<span<u8>, 3>;

        Mappings mappings;
        texture::Dimensions dimensions{};
        texture::Format format{};
        texture::TileConfig tileConfig{};
        vk::ImageViewType viewType{};
        u32 baseArrayLayer{};
        u32 layerCount{1};
        u32 layerStride{};
        u32 mipLevelCount{1};
        u32 viewMipBase{};
        u32 viewMipCount{1};
        vk::ComponentMapping swizzle{};
        vk::ImageAspectFlags aspect{};

        GuestTexture() {}

        GuestTexture(Mappings mappings, texture::Dimensions dimensions, texture::Format format, texture::TileConfig tileConfig, vk::ImageViewType viewType, u32 baseArrayLayer = 0, u32 layerCount = 1, u32 layerStride = 0, u32 mipLevelCount = 1, u32 viewMipBase = 0, u32 viewMipCount = 1)
            : mappings(mappings),
              dimensions(dimensions),
              format(format),
              tileConfig(tileConfig),
              viewType(viewType),
              baseArrayLayer(baseArrayLayer),
              layerCount(layerCount),
              layerStride(layerStride),
              mipLevelCount(mipLevelCount),
              viewMipBase(viewMipBase),
              viewMipCount(viewMipCount),
              aspect(format->vkAspect) {}

        GuestTexture(span<u8> mapping, texture::Dimensions dimensions, texture::Format format, texture::TileConfig tileConfig, vk::ImageViewType viewType, u32 baseArrayLayer = 0, u32 layerCount = 1, u32 layerStride = 0, u32 mipLevelCount = 1, u32 viewMipBase = 0, u32 viewMipCount = 1)
            : mappings(1, mapping),
              dimensions(dimensions),
              format(format),
              tileConfig(tileConfig),
              viewType(viewType),
              baseArrayLayer(baseArrayLayer),
              layerCount(layerCount),
              layerStride(layerStride),
              mipLevelCount(mipLevelCount),
              viewMipBase(viewMipBase),
              viewMipCount(viewMipCount),
              aspect(format->vkAspect) {}

        u32 GetLayerStride();

        u32 CalculateLayerSize() const;

        vk::ImageType GetImageType() const;

        u32 GetViewLayerCount() const;

        u32 GetViewDepth() const;

        size_t GetSize();

        bool MappingsValid() const;
    };

    class TextureManager;

    class TextureView : public std::enable_shared_from_this<TextureView> {
      private:
        vk::ImageView vkView{};

      public:
        LockableSharedPtr<Texture> texture;
        vk::ImageViewType type;
        texture::Format format;
        vk::ComponentMapping mapping;
        vk::ImageSubresourceRange range;

        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format = {}, vk::ComponentMapping mapping = {});

        void lock();

        bool LockWithTag(ContextTag tag);

        void unlock();

        bool try_lock();

        vk::ImageView GetView();

        bool operator==(const TextureView &rhs) {
            return texture == rhs.texture && type == rhs.type && format == rhs.format && mapping == rhs.mapping && range == rhs.range;
        }
    };

    class Texture : public std::enable_shared_from_this<Texture> {
      private:
        GPU &gpu;
        RecursiveSpinLock mutex;
        std::atomic<ContextTag> tag{};
        std::condition_variable_any backingCondition;
        using BackingType = std::variant<vk::Image, vk::raii::Image, memory::Image>;
        BackingType backing;

        span<u8> mirror{};
        span<u8> alignedMirror{};
        std::optional<TrapHandle> trapHandle{};
        enum class DirtyState {
            Clean,
            CpuDirty,
            GpuDirty,
        } dirtyState{DirtyState::CpuDirty};
        bool memoryFreed{};
        std::recursive_mutex stateMutex;

        struct TextureViewStorage {
            vk::ImageViewType type;
            texture::Format format;
            vk::ComponentMapping mapping;
            vk::ImageSubresourceRange range;
            vk::raii::ImageView vkView;

            TextureViewStorage(vk::ImageViewType type, texture::Format format, vk::ComponentMapping mapping, vk::ImageSubresourceRange range, vk::raii::ImageView &&vkView);
        };

        std::vector<TextureViewStorage> views;

        std::shared_ptr<memory::StagingBuffer> downloadStagingBuffer{};

        u32 lastRenderPassIndex{};
        texture::RenderPassUsage lastRenderPassUsage{texture::RenderPassUsage::None};
        bool everUsedAsRt{};
        vk::PipelineStageFlags pendingStageMask{};
        vk::PipelineStageFlags readStageMask{};

        friend TextureManager;
        friend TextureView;

        /**
         * @brief Sets up mirror mappings for the guest mappings, this must be called after construction for the mirror to be valid
         */
        void SetupGuestMappings();

        /**
         * @brief Creates the contiguous mirror of the guest mappings, used to restore the mirror after FreeGuest has been called
         */
        void CreateGuestMirror();

        /**
         * @brief An implementation function for guest -> host texture synchronization
         */
        std::shared_ptr<memory::StagingBuffer> SynchronizeHostImpl();

        /**
         * @brief Records commands for copying data from a staging buffer to the texture's backing
         */
        void CopyFromStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer);

        /**
         * @brief Records commands for copying data from the texture's backing to a staging buffer
         */
        void CopyIntoStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer);

        /**
         * @brief Copies data from the supplied host buffer into the guest texture
         */
        void CopyToGuest(u8 *hostBuffer);

        /**
         * @brief Frees the guest side copy of the texture
         */
        void FreeGuest();

        /**
         * @return A vector of all the buffer image copies for the texture
         */
        boost::container::small_vector<vk::BufferImageCopy, 10> GetBufferImageCopies();

        static constexpr size_t FrequentlyLockedThreshold{2};
        size_t accumulatedCpuLockCounter{};

        static constexpr size_t SkipReadbackHackWaitCountThreshold{6};
        static constexpr std::chrono::nanoseconds SkipReadbackHackWaitTimeThreshold{constant::NsInSecond / 4};
        size_t accumulatedGuestWaitCounter{};
        std::chrono::nanoseconds accumulatedGuestWaitTime{};

      public:
        std::shared_ptr<FenceCycle> cycle;
        std::optional<GuestTexture> guest;
        texture::Dimensions dimensions;
        texture::Format format;
        vk::ImageLayout layout;
        vk::ImageTiling tiling;
        vk::ImageCreateFlags flags;
        vk::ImageUsageFlags usage;
        u32 layerCount;
        size_t deswizzledLayerStride{};
        size_t layerStride{};
        u32 levelCount;
        std::vector<texture::MipLevelLayout> mipLayouts;
        size_t deswizzledSurfaceSize{};
        size_t surfaceSize{};
        vk::SampleCountFlagBits sampleCount;
        bool replaced{};

        Texture(GPU &gpu, BackingType &&backing, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, vk::ImageCreateFlags flags, vk::ImageUsageFlags usage, u32 levelCount = 1, u32 layerCount = 1, vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1);

        Texture(GPU &gpu, GuestTexture guest);

        ~Texture();

        constexpr vk::Image GetBacking() {
            return std::visit(VariantVisitor{
                [](vk::Image image) { return image; },
                [](const vk::raii::Image &image) { return *image; },
                [](const memory::Image &image) { return image.vkImage; },
            }, backing);
        }

        void lock();

        bool LockWithTag(ContextTag tag);

        void unlock();

        bool try_lock();

        bool WaitOnBacking();

        void WaitOnFence();

        void SwapBacking(BackingType &&backing, vk::ImageLayout layout = vk::ImageLayout::eUndefined);

        void TransitionLayout(vk::ImageLayout layout);

        void MarkGpuDirty(UsageTracker &usageTracker);

        void SynchronizeHost(bool gpuDirty = false);

        void SynchronizeHostInline(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle, bool gpuDirty = false);

        void SynchronizeGuest(bool cpuDirty = false, bool skipTrap = false);

        std::shared_ptr<TextureView> GetView(vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format = {}, vk::ComponentMapping mapping = {});

        void CopyFrom(std::shared_ptr<Texture> source, vk::Semaphore waitSemaphore, vk::Semaphore signalSemaphore, texture::Format srcFormat, const vk::ImageSubresourceRange &subresource = vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        });

        bool FrequentlyLocked() {
            return accumulatedCpuLockCounter >= FrequentlyLockedThreshold;
        }

        bool ValidateRenderPassUsage(u32 renderPassIndex, texture::RenderPassUsage renderPassUsage);

        void UpdateRenderPassUsage(u32 renderPassIndex, texture::RenderPassUsage renderPassUsage);

        texture::RenderPassUsage GetLastRenderPassUsage();

        vk::PipelineStageFlags GetReadStageMask();

        void PopulateReadBarrier(vk::PipelineStageFlagBits dstStage, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask);
    };
}
