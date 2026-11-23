// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include "memory_manager.h"
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace skyline::gpu::memory {
    /**
     * @brief If the result isn't VK_SUCCESS then an exception is thrown
     */
    void ThrowOnFail(VkResult result, const char *function = __builtin_FUNCTION()) {
        if (result != VK_SUCCESS)
            vk::throwResultException(vk::Result(result), function);
    }

    Buffer::~Buffer() {
        if (vmaAllocator && vmaAllocation && vkBuffer) {
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(vmaAllocator, vmaAllocation, &info);
            diagnostics::counters.buffersLive.fetch_sub(1, std::memory_order_relaxed);
            diagnostics::counters.buffersBytes.fetch_sub(info.size, std::memory_order_relaxed);
#endif
            vmaDestroyBuffer(vmaAllocator, vkBuffer, vmaAllocation);
        }
    }

    Image::~Image() {
        if (vmaAllocator && vmaAllocation && vkImage) {
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(vmaAllocator, vmaAllocation, &info);
            diagnostics::counters.imagesLive.fetch_sub(1, std::memory_order_relaxed);
            diagnostics::counters.imagesBytes.fetch_sub(info.size, std::memory_order_relaxed);
#endif
            if (pointer)
                vmaUnmapMemory(vmaAllocator, vmaAllocation);
            vmaDestroyImage(vmaAllocator, vkImage, vmaAllocation);
        }
    }

    u8 *Image::data() {
        if (pointer) [[likely]]
            return pointer;
        ThrowOnFail(vmaMapMemory(vmaAllocator, vmaAllocation, reinterpret_cast<void **>(&pointer)));
        return pointer;
    }

    MemoryManager::MemoryManager(GPU &pGpu) : gpu{pGpu} {
        auto instanceDispatcher{gpu.vkInstance.getDispatcher()};
        auto deviceDispatcher{gpu.vkDevice.getDispatcher()};
        VmaVulkanFunctions vulkanFunctions{
            .vkGetPhysicalDeviceProperties = instanceDispatcher->vkGetPhysicalDeviceProperties,
            .vkGetPhysicalDeviceMemoryProperties = instanceDispatcher->vkGetPhysicalDeviceMemoryProperties,
            .vkAllocateMemory = deviceDispatcher->vkAllocateMemory,
            .vkFreeMemory = deviceDispatcher->vkFreeMemory,
            .vkMapMemory = deviceDispatcher->vkMapMemory,
            .vkUnmapMemory = deviceDispatcher->vkUnmapMemory,
            .vkFlushMappedMemoryRanges = deviceDispatcher->vkFlushMappedMemoryRanges,
            .vkInvalidateMappedMemoryRanges = deviceDispatcher->vkInvalidateMappedMemoryRanges,
            .vkBindBufferMemory = deviceDispatcher->vkBindBufferMemory,
            .vkBindImageMemory = deviceDispatcher->vkBindImageMemory,
            .vkGetBufferMemoryRequirements = deviceDispatcher->vkGetBufferMemoryRequirements,
            .vkGetImageMemoryRequirements = deviceDispatcher->vkGetImageMemoryRequirements,
            .vkCreateBuffer = deviceDispatcher->vkCreateBuffer,
            .vkDestroyBuffer = deviceDispatcher->vkDestroyBuffer,
            .vkCreateImage = deviceDispatcher->vkCreateImage,
            .vkDestroyImage = deviceDispatcher->vkDestroyImage,
            .vkCmdCopyBuffer = deviceDispatcher->vkCmdCopyBuffer,
            .vkGetBufferMemoryRequirements2KHR = deviceDispatcher->vkGetBufferMemoryRequirements2,
            .vkGetImageMemoryRequirements2KHR = deviceDispatcher->vkGetImageMemoryRequirements2,
            .vkBindBufferMemory2KHR = deviceDispatcher->vkBindBufferMemory2,
            .vkBindImageMemory2KHR = deviceDispatcher->vkBindImageMemory2,
            .vkGetPhysicalDeviceMemoryProperties2KHR = instanceDispatcher->vkGetPhysicalDeviceMemoryProperties2,
        };
        VmaAllocatorCreateInfo allocatorCreateInfo{
            .physicalDevice = *gpu.vkPhysicalDevice,
            .device = *gpu.vkDevice,
            .instance = *gpu.vkInstance,
            .pVulkanFunctions = &vulkanFunctions,
            .vulkanApiVersion = VkApiVersion,
        };
        ThrowOnFail(vmaCreateAllocator(&allocatorCreateInfo, &vmaAllocator));
    }

    MemoryManager::~MemoryManager() {
        vmaDestroyAllocator(vmaAllocator);
    }

    std::shared_ptr<StagingBuffer> MemoryManager::AllocateStagingBuffer(vk::DeviceSize size) {
        vk::BufferCreateInfo bufferCreateInfo{
            .size = size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
        };
        VmaAllocationCreateInfo allocationCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_CPU_ONLY,
        };

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateBuffer(vmaAllocator, &static_cast<const VkBufferCreateInfo &>(bufferCreateInfo), &allocationCreateInfo, &buffer, &allocation, &allocationInfo));

        auto staging{std::make_shared<memory::StagingBuffer>(reinterpret_cast<u8 *>(allocationInfo.pMappedData), size, vmaAllocator, buffer, allocation)};
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
        diagnostics::counters.buffersLive.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.buffersBytes.fetch_add(allocationInfo.size, std::memory_order_relaxed);
        diagnostics::counters.buffersCreated.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.stagingLive.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.stagingBytes.fetch_add(size, std::memory_order_relaxed);
        diagnostics::counters.stagingCreated.fetch_add(1, std::memory_order_relaxed);
#endif
        return staging;
    }

    Buffer MemoryManager::AllocateBuffer(vk::DeviceSize size) {
        vk::BufferUsageFlags usage{
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer |
            vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
            vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransformFeedbackBufferEXT
        };
        if (gpu.traits.supportsConditionalRendering)
            usage |= vk::BufferUsageFlagBits::eConditionalRenderingEXT;

        vk::BufferCreateInfo bufferCreateInfo{
            .size = size,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
        };
        VmaAllocationCreateInfo allocationCreateInfo{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_UNKNOWN,
            .requiredFlags = static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eDeviceLocal),
        };

        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateBuffer(vmaAllocator, &static_cast<const VkBufferCreateInfo &>(bufferCreateInfo), &allocationCreateInfo, &buffer, &allocation, &allocationInfo));

#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
        diagnostics::counters.buffersLive.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.buffersBytes.fetch_add(allocationInfo.size, std::memory_order_relaxed);
        diagnostics::counters.buffersCreated.fetch_add(1, std::memory_order_relaxed);
#endif
        return Buffer(reinterpret_cast<u8 *>(allocationInfo.pMappedData), size, vmaAllocator, buffer, allocation);
    }

    Image MemoryManager::AllocateImage(const vk::ImageCreateInfo &createInfo) {
        VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_GPU_ONLY,
        };

        VkImage image;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateImage(vmaAllocator, &static_cast<const VkImageCreateInfo &>(createInfo), &allocationCreateInfo, &image, &allocation, &allocationInfo));

#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
        diagnostics::counters.imagesLive.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.imagesBytes.fetch_add(allocationInfo.size, std::memory_order_relaxed);
        diagnostics::counters.imagesCreated.fetch_add(1, std::memory_order_relaxed);
#endif
        return Image(vmaAllocator, image, allocation);
    }

    Image MemoryManager::AllocateMappedImage(const vk::ImageCreateInfo &createInfo) {
        VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_UNKNOWN,
            .requiredFlags = static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eDeviceLocal),
        };

        VkImage image;
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;
        ThrowOnFail(vmaCreateImage(vmaAllocator, &static_cast<const VkImageCreateInfo &>(createInfo), &allocationCreateInfo, &image, &allocation, &allocationInfo));

#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
        diagnostics::counters.imagesLive.fetch_add(1, std::memory_order_relaxed);
        diagnostics::counters.imagesBytes.fetch_add(allocationInfo.size, std::memory_order_relaxed);
        diagnostics::counters.imagesCreated.fetch_add(1, std::memory_order_relaxed);
#endif
        return Image(vmaAllocator, image, allocation);
    }

    ImportedBuffer MemoryManager::ImportBuffer(span<u8> cpuMapping) {
        if (!gpu.traits.supportsAdrenoDirectMemoryImport)
            throw exception("Cannot import host buffers without adrenotools import support!");

        if (!adrenotools_import_user_mem(&gpu.adrenotoolsImportMapping, cpuMapping.data(), cpuMapping.size()))
            throw exception("Failed to import user memory");

        vk::BufferUsageFlags usage{
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer |
            vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
            vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransformFeedbackBufferEXT
        };
        if (gpu.traits.supportsConditionalRendering)
            usage |= vk::BufferUsageFlagBits::eConditionalRenderingEXT;

        auto buffer{gpu.vkDevice.createBuffer(vk::BufferCreateInfo{
            .size = cpuMapping.size(),
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive
        })};

        auto memory{gpu.vkDevice.allocateMemory(vk::MemoryAllocateInfo{
            .allocationSize = cpuMapping.size(),
            .memoryTypeIndex = gpu.traits.hostVisibleCoherentCachedMemoryType,
        })};

        if (!adrenotools_validate_gpu_mapping(&gpu.adrenotoolsImportMapping))
            throw exception("Failed to validate GPU mapping");

        gpu.vkDevice.bindBufferMemory2({vk::BindBufferMemoryInfo{
            .buffer = *buffer,
            .memory = *memory,
            .memoryOffset = 0
        }});

        return ImportedBuffer{cpuMapping, std::move(buffer), std::move(memory)};
    }
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
    void MemoryManager::LogMemoryDiagnostics() {
        using Clock = std::chrono::steady_clock;
        const auto now{Clock::now()};
        const auto nowMs{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()};
        auto expected{nextDiagnosticSnapshotMs.load(std::memory_order_relaxed)};
        if (nowMs < expected || !nextDiagnosticSnapshotMs.compare_exchange_strong(expected, nowMs + 15000, std::memory_order_relaxed))
            return;

        // Diagnostics must never terminate emulation. The VMA figures measure Vulkan
        // allocator usage, not all physical RSS, and the categories below overlap.
        try {
            std::uint64_t rssKiB{}, anonKiB{}, fileKiB{}, shmemKiB{}, swapKiB{};
            std::ifstream status{"/proc/self/status"};
            for (std::string line; std::getline(status, line);) {
                auto value = [&](const char *key) -> std::uint64_t {
                    const std::string_view prefix{key};
                    if (line.compare(0, prefix.size(), prefix) != 0)
                        return 0;
                    std::istringstream stream{line.substr(prefix.size())};
                    std::uint64_t kib{};
                    stream >> kib;
                    return kib;
                };
                if (line.rfind("VmRSS:", 0) == 0) rssKiB = value("VmRSS:");
                else if (line.rfind("RssAnon:", 0) == 0) anonKiB = value("RssAnon:");
                else if (line.rfind("RssFile:", 0) == 0) fileKiB = value("RssFile:");
                else if (line.rfind("RssShmem:", 0) == 0) shmemKiB = value("RssShmem:");
                else if (line.rfind("VmSwap:", 0) == 0) swapKiB = value("VmSwap:");
            }

            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(vmaAllocator, budgets.data());
            const auto properties{gpu.vkPhysicalDevice.getMemoryProperties()};
            std::uint64_t allocatedBytes{}, blockBytes{}, allocations{};
            for (std::uint32_t heap{}; heap < properties.memoryHeapCount; ++heap) {
                allocatedBytes += budgets[heap].statistics.allocationBytes;
                blockBytes += budgets[heap].statistics.blockBytes;
                allocations += budgets[heap].statistics.allocationCount;
            }

            const auto elapsedUs{std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count()};
            LOGI("[GPU-MEM] clock_ms={} rss_kib={} anon_kib={} file_kib={} shmem_kib={} swap_kib={} vma_alloc_kib={} vma_blocks_kib={} vma_allocations={} vma_images_live={} vma_images_kib={} vma_images_created={} vma_buffers_live={} vma_buffers_kib={} vma_buffers_created={} textures_live={} textures_created={} textures_destroyed={} mapping_entries={} mapping_guest_kib={} replaced_mark_events={} staging_live={} staging_kib={} staging_created={} megachunks_live={} megachunks_mib={} megachunks_created={} sample_us={}",
                nowMs, rssKiB, anonKiB, fileKiB, shmemKiB, swapKiB, allocatedBytes / 1024, blockBytes / 1024, allocations,
                diagnostics::counters.imagesLive.load(std::memory_order_relaxed),
                diagnostics::counters.imagesBytes.load(std::memory_order_relaxed) / 1024,
                diagnostics::counters.imagesCreated.load(std::memory_order_relaxed),
                diagnostics::counters.buffersLive.load(std::memory_order_relaxed),
                diagnostics::counters.buffersBytes.load(std::memory_order_relaxed) / 1024,
                diagnostics::counters.buffersCreated.load(std::memory_order_relaxed),
                diagnostics::counters.texturesLive.load(std::memory_order_relaxed),
                diagnostics::counters.texturesCreated.load(std::memory_order_relaxed),
                diagnostics::counters.texturesDestroyed.load(std::memory_order_relaxed),
                diagnostics::counters.mappingEntries.load(std::memory_order_relaxed),
                diagnostics::counters.mappingGuestBytes.load(std::memory_order_relaxed) / 1024,
                diagnostics::counters.replacedMarkEvents.load(std::memory_order_relaxed),
                diagnostics::counters.stagingLive.load(std::memory_order_relaxed),
                diagnostics::counters.stagingBytes.load(std::memory_order_relaxed) / 1024,
                diagnostics::counters.stagingCreated.load(std::memory_order_relaxed),
                diagnostics::counters.megabufferChunksLive.load(std::memory_order_relaxed),
                diagnostics::counters.megabufferChunksLive.load(std::memory_order_relaxed) * MegaBufferChunkSize / (1024 * 1024),
                diagnostics::counters.megabufferChunksCreated.load(std::memory_order_relaxed),
                elapsedUs);
        } catch (...) {
            LOGW("[GPU-MEM] snapshot_failed");
        }
    }
#endif
}
