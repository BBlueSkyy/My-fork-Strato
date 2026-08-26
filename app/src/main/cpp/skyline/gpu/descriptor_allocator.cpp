// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <limits>
#include <gpu.h>
#include "descriptor_allocator.h"

namespace skyline::gpu {
    DescriptorAllocator::DescriptorSetSlot::DescriptorSetSlot(vk::DescriptorSet descriptorSet) : descriptorSet{descriptorSet} {}

    DescriptorAllocator::DescriptorSetSlot::DescriptorSetSlot(DescriptorAllocator::DescriptorSetSlot &&other) : descriptorSet{other.descriptorSet} {
        other.descriptorSet = nullptr;
    }

    DescriptorAllocator::DescriptorPool::DescriptorPool(const vk::raii::Device &device, const vk::DescriptorPoolCreateInfo &createInfo, DescriptorCounts pDescriptorCounts)
        : vk::raii::DescriptorPool{device, createInfo},
          freeSetCount{createInfo.maxSets},
          remainingDescriptorCounts{pDescriptorCounts} {}

    void DescriptorAllocator::AllocateDescriptorPool() {
        using DescriptorSizes = std::array<vk::DescriptorPoolSize, 6>;

        DescriptorSizes descriptorSizes{
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[0],
                .type = vk::DescriptorType::eUniformBuffer,
            },
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[1],
                .type = vk::DescriptorType::eStorageBuffer,
            },
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[2],
                .type = vk::DescriptorType::eCombinedImageSampler,
            },
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[3],
                .type = vk::DescriptorType::eStorageImage,
            },
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[4],
                .type = vk::DescriptorType::eUniformTexelBuffer,
            },
            vk::DescriptorPoolSize{
                .descriptorCount = descriptorCounts[5],
                .type = vk::DescriptorType::eStorageTexelBuffer,
            }
        };

        pool = std::make_shared<DescriptorPool>(gpu.vkDevice, vk::DescriptorPoolCreateInfo{
            .maxSets = descriptorSetCount,
            .pPoolSizes = descriptorSizes.data(),
            .poolSizeCount = descriptorSizes.size(),
        }, descriptorCounts);

        LOGI("Created descriptor pool: sets={}, UBO={}, SSBO={}, sampled={}, storageImage={}, uniformTexel={}, storageTexel={}",
             descriptorSetCount, descriptorCounts[0], descriptorCounts[1], descriptorCounts[2], descriptorCounts[3], descriptorCounts[4], descriptorCounts[5]);
    }

    DescriptorAllocator::DescriptorCounts DescriptorAllocator::GetDescriptorCounts(span<const vk::DescriptorSetLayoutBinding> layoutBindings) {
        DescriptorCounts requirements{};

        for (const auto &binding : layoutBindings) {
            size_t index;
            switch (binding.descriptorType) {
                case vk::DescriptorType::eUniformBuffer:
                    index = 0;
                    break;
                case vk::DescriptorType::eStorageBuffer:
                    index = 1;
                    break;
                case vk::DescriptorType::eCombinedImageSampler:
                    index = 2;
                    break;
                case vk::DescriptorType::eStorageImage:
                    index = 3;
                    break;
                case vk::DescriptorType::eUniformTexelBuffer:
                    index = 4;
                    break;
                case vk::DescriptorType::eStorageTexelBuffer:
                    index = 5;
                    break;
                default:
                    throw exception("Unsupported descriptor type in descriptor allocator: {}", static_cast<u32>(binding.descriptorType));
            }

            if (binding.descriptorCount > std::numeric_limits<u32>::max() - requirements[index])
                throw exception("Descriptor count overflow for type: {}", static_cast<u32>(binding.descriptorType));

            requirements[index] += binding.descriptorCount;
        }

        return requirements;
    }

    bool DescriptorAllocator::UpdateDescriptorCounts(const DescriptorCounts &requirements) {
        bool changed{};

        for (size_t i{}; i < descriptorCounts.size(); i++) {
            maxDescriptorCountsPerSet[i] = std::max(maxDescriptorCountsPerSet[i], requirements[i]);

            u64 requiredCount{static_cast<u64>(maxDescriptorCountsPerSet[i]) * descriptorSetCount};
            if (requiredCount > std::numeric_limits<u32>::max())
                throw exception("Descriptor pool size overflow for descriptor type index: {}", i);

            u32 targetCount{std::max(BaseDescriptorCounts[i], static_cast<u32>(requiredCount))};
            if (descriptorCounts[i] < targetCount) {
                u32 grownCount{descriptorCounts[i]};
                while (grownCount < targetCount) {
                    if (grownCount > std::numeric_limits<u32>::max() / 2) {
                        grownCount = targetCount;
                        break;
                    }

                    grownCount *= 2;
                }

                descriptorCounts[i] = grownCount;
                changed = true;
            }
        }

        return changed;
    }

    void DescriptorAllocator::GrowDescriptorCounts() {
        for (auto &count : descriptorCounts) {
            if (count > std::numeric_limits<u32>::max() / 2)
                throw exception("Descriptor pool size overflow while growing pool");

            count *= 2;
        }
    }

    vk::ResultValue<vk::DescriptorSet> DescriptorAllocator::AllocateVkDescriptorSet(vk::DescriptorSetLayout layout, const DescriptorCounts &requirements) {
        vk::DescriptorSetAllocateInfo allocateInfo{
            .descriptorPool = **pool,
            .pSetLayouts = &layout,
            .descriptorSetCount = 1,
        };
        vk::DescriptorSet descriptorSet{};

        auto result{(*gpu.vkDevice).allocateDescriptorSets(&allocateInfo, &descriptorSet, *gpu.vkDevice.getDispatcher())};
        if (result == vk::Result::eSuccess) {
            pool->freeSetCount--;

            for (size_t i{}; i < requirements.size(); i++)
                pool->remainingDescriptorCounts[i] -= requirements[i];
        }

        return vk::createResultValue(result, descriptorSet, __builtin_FUNCTION(), {
            vk::Result::eSuccess,
            vk::Result::eErrorOutOfPoolMemory,
            vk::Result::eErrorFragmentedPool
        });
    }

    DescriptorAllocator::ActiveDescriptorSet::ActiveDescriptorSet(std::shared_ptr<DescriptorPool> pPool, DescriptorSetSlot *slot) : pool{std::move(pPool)}, slot{slot} {}

    DescriptorAllocator::ActiveDescriptorSet::ActiveDescriptorSet(DescriptorAllocator::ActiveDescriptorSet &&other) noexcept {
        pool = std::move(other.pool);
        slot = std::exchange(other.slot, nullptr);
    }

    DescriptorAllocator::ActiveDescriptorSet::~ActiveDescriptorSet() {
        if (slot)
            slot->active.clear(std::memory_order_release);
    }

    DescriptorAllocator::DescriptorAllocator(GPU &gpu) : gpu{gpu} {
        AllocateDescriptorPool();
    }

    DescriptorAllocator::ActiveDescriptorSet DescriptorAllocator::AllocateSet(vk::DescriptorSetLayout layout, span<const vk::DescriptorSetLayoutBinding> layoutBindings) {
        std::scoped_lock allocatorLock{mutex};

        DescriptorCounts requirements{GetDescriptorCounts(layoutBindings)};
        auto it{pool->layoutSlots.find(layout)};
        if (it != pool->layoutSlots.end()) {
            for (auto slotIt{it->second.begin()} ; slotIt != it->second.end() ; slotIt++) {
                if (!slotIt->active.test_and_set(std::memory_order_acq_rel)) {
                    // Move active slots to end of list to reduce search time
                    it->second.splice(it->second.end(), it->second, slotIt);
                    return ActiveDescriptorSet{pool, &*slotIt};
                }
            }
        }

        // Size the pool for a full batch of the most demanding layout observed before asking the driver to allocate.
        if (UpdateDescriptorCounts(requirements))
            AllocateDescriptorPool();

        // Maxwell3D may keep a complete descriptor batch alive. Grow proactively instead of using a Vulkan error as flow control.
        if (pool->freeSetCount == 0) {
            if (descriptorSetCount > std::numeric_limits<u32>::max() / 2)
                throw exception("Descriptor set count overflow while growing pool");

            descriptorSetCount *= 2;
            UpdateDescriptorCounts(requirements);
            AllocateDescriptorPool();
        }

        for (size_t i{}; i < requirements.size(); i++) {
            if (requirements[i] > pool->remainingDescriptorCounts[i]) {
                // This should only be reachable with unusual driver accounting or a mixed-layout corner case.
                AllocateDescriptorPool();
                break;
            }
        }

        while (true) {
            auto set{AllocateVkDescriptorSet(layout, requirements)};
            if (set.result == vk::Result::eSuccess) {
                auto &layoutSlots{pool->layoutSlots.try_emplace(layout).first->second};
                auto &slot{layoutSlots.emplace_back(set.value)};
                slot.active.test_and_set(std::memory_order_release);
                return ActiveDescriptorSet{pool, &slot};
            }

            if (set.result == vk::Result::eErrorOutOfPoolMemory) {
                // Some drivers may account for descriptors more strictly than advertised. Keep a defensive exponential fallback.
                GrowDescriptorCounts();
                AllocateDescriptorPool();
            } else if (set.result == vk::Result::eErrorFragmentedPool) {
                AllocateDescriptorPool();
            } else {
                throw exception("Unexpected descriptor set allocation result: {}", static_cast<i32>(set.result));
            }
        }
    }
}
