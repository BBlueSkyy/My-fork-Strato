// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace skyline::gpu::interconnect {
    /**
     * @brief One `VkWriteDescriptorSet` worth of consecutive descriptors planned by `PlanDescriptorWrites`
     */
    struct DescriptorWriteRange {
        std::uint32_t dstBinding;      //!< First binding the write targets (always array element 0 of that binding)
        std::uint32_t descriptorCount; //!< Number of descriptors in the write, may spill over into following bindings when batched
        std::uint32_t firstElement;    //!< Index of the first descriptor of this write in the flat per-type descriptor array
    };

    /**
     * @brief The number of `VkWriteDescriptorSet` entries `PlanDescriptorWrites` emits for `descCount` bindings of one descriptor type
     */
    constexpr std::uint32_t DescriptorWriteCount(std::size_t descCount, bool individual) {
        return static_cast<std::uint32_t>(individual ? descCount : (descCount > 0 ? 1 : 0));
    }

    /**
     * @brief Splits the descriptors of a single Vulkan descriptor type into descriptor set writes
     * @param descs Bindings of this type, laid out in consecutive bindings starting from `firstBinding`, each providing a `count` of array elements
     * @param individual If every binding must be written by its own `VkWriteDescriptorSet` rather than one write spilling across all of the consecutive bindings
     * @param emit Invoked once per planned write with a `DescriptorWriteRange`
     * @return The number of writes emitted, always equal to `DescriptorWriteCount(descs.size(), individual)`
     * @note Batched writes are valid Vulkan but the Adreno proprietary driver crashes in vkUpdateDescriptorSets when a single write spans
     *       multiple texel buffer bindings (see `QuirkManager::needsIndividualTexelBufferBindingWrites`)
     */
    template<typename Descs, typename Emit>
    constexpr std::uint32_t PlanDescriptorWrites(const Descs &descs, std::uint32_t firstBinding, bool individual, Emit &&emit) {
        if (descs.empty())
            return 0;

        if (!individual) {
            std::uint32_t total{};
            for (const auto &desc : descs)
                total += static_cast<std::uint32_t>(desc.count);

            emit(DescriptorWriteRange{firstBinding, total, 0});
            return 1;
        }

        std::uint32_t binding{firstBinding};
        std::uint32_t element{};
        for (const auto &desc : descs) {
            emit(DescriptorWriteRange{binding++, static_cast<std::uint32_t>(desc.count), element});
            element += static_cast<std::uint32_t>(desc.count);
        }

        return static_cast<std::uint32_t>(descs.size());
    }
}
