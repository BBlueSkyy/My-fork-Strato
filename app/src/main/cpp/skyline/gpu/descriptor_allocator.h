// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <vulkan/vulkan_raii.hpp>
#include <common/spin_lock.h>
#include <common.h>

namespace skyline::gpu {
    /**
     * @brief A dynamic descriptor set allocator with internal resizing of the descriptor pool to size up to allocation demand
     */
    class DescriptorAllocator {
      private:
        GPU &gpu;
        SpinLock mutex; //!< Synchronizes the creation and replacement of the pool object

        using DescriptorCounts = std::array<u32, 6>;

        static constexpr u32 InitialDescriptorSetCount{0x100}; //!< Matches the largest batch of descriptor sets kept alive by Maxwell3D
        static constexpr DescriptorCounts BaseDescriptorCounts{512, 64, 256, 16, 4, 4};

        u32 descriptorSetCount{InitialDescriptorSetCount}; //!< The maximum amount of descriptor sets in the current pool
        DescriptorCounts maxDescriptorCountsPerSet{}; //!< Maximum descriptor demand of every layout observed so far
        DescriptorCounts descriptorCounts{BaseDescriptorCounts}; //!< Descriptor capacity of the current pool

        /**
         * @brief A slot representing a single descriptor set dynamically allocated from the pool
         */
        struct DescriptorSetSlot {
            std::atomic_flag active{true}; //!< If the descriptor is currently being utilized
            vk::DescriptorSet descriptorSet; //!< The descriptor set allocated from the pool

            DescriptorSetSlot(vk::DescriptorSet descriptorSet);

            DescriptorSetSlot(DescriptorSetSlot &&other);
        };

        /**
         * @brief A lockable VkDescriptorPool for maintaining external synchronization requirements
         */
        struct DescriptorPool : public vk::raii::DescriptorPool {
            size_t freeSetCount{}; //!< The amount of sets free to allocate from this pool
            DescriptorCounts remainingDescriptorCounts{}; //!< Remaining descriptors of each supported type
            std::unordered_map<vk::DescriptorSetLayout, std::list<DescriptorSetSlot>> layoutSlots; //!< A map of pools based on the layout of the descriptor sets

            DescriptorPool(vk::raii::Device const &device, vk::DescriptorPoolCreateInfo const &createInfo, DescriptorCounts descriptorCounts);
        };

        std::shared_ptr<DescriptorPool> pool; //!< The current pool used by any allocations in the class, replaced when an error is ran into

        /**
         * @brief (Re-)Allocates the descriptor pool with the current descriptor capacities and set count
         * @note `DescriptorAllocator::mutex` **must** be locked prior to calling this
         */
        void AllocateDescriptorPool();

        /**
         * @brief Calculates how many descriptors of each supported type a layout consumes per set
         */
        static DescriptorCounts GetDescriptorCounts(span<const vk::DescriptorSetLayoutBinding> layoutBindings);

        /**
         * @brief Ensures a fresh pool can hold descriptorSetCount instances of every observed layout
         * @return True when the pool capacities changed
         */
        bool UpdateDescriptorCounts(const DescriptorCounts &requirements);

        /**
         * @brief Doubles all descriptor capacities after an unexpected driver-side pool exhaustion
         */
        void GrowDescriptorCounts();

        /**
         * @brief Allocates a descriptor set with the specified layout from the pool
         * @return An error code that's either `eSuccess`, `eErrorOutOfPoolMemory` or `eErrorFragmentedPool`
         */
        vk::ResultValue<vk::DescriptorSet> AllocateVkDescriptorSet(vk::DescriptorSetLayout layout, const DescriptorCounts &requirements);

      public:
        /**
         * @brief A RAII-bound descriptor set that automatically frees resources into the pool on destruction while respecting external synchronization requirements
         */
        struct ActiveDescriptorSet {
          private:
            std::shared_ptr<DescriptorPool> pool;
            DescriptorSetSlot *slot;

            friend class DescriptorAllocator;

            ActiveDescriptorSet(std::shared_ptr<DescriptorPool> pool, DescriptorSetSlot *slot);

          public:
            ActiveDescriptorSet(ActiveDescriptorSet &&other) noexcept;

            /* Delete the copy constructor/assignment to prevent early freeing of the descriptor set */
            ActiveDescriptorSet(const ActiveDescriptorSet &) = delete;

            ActiveDescriptorSet &operator=(const ActiveDescriptorSet &) = delete;

            ~ActiveDescriptorSet();

            vk::DescriptorSet &operator*() const {
                return slot->descriptorSet;
            }
        };

        DescriptorAllocator(GPU &gpu);

        /**
         * @brief Allocates a descriptor set from the pool with the supplied layout
         * @note The layout object must be reused for equivalent layouts to avoid unnecessary descriptor set creation
         * @note layoutBindings must exactly describe the supplied layout
         * @note The supplied ActiveDescriptorSet **must** stay alive until the descriptor set can be freed, it must not be destroyed after being bound but after any associated commands have completed execution
         */
        ActiveDescriptorSet AllocateSet(vk::DescriptorSetLayout layout, span<const vk::DescriptorSetLayoutBinding> layoutBindings);
    };
}
