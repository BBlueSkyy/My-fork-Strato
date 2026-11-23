// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace skyline::gpu {
    /**
     * Device-local immutable layout objects. Only the single-set, extension-free
     * interfaces emitted by the pipeline builders are interned. Extended create
     * infos and immutable samplers retain the original uncached creation path:
     * sampler handle reuse and extension-specific state cannot alias a cache key.
     */
    class PipelineLayoutCache {
      public:
        struct DescriptorLayout {
            vk::raii::DescriptorSetLayout layout;

            DescriptorLayout(const vk::raii::Device &device, const vk::DescriptorSetLayoutCreateInfo &info)
                : layout{device, info} {}
        };

        struct Layouts {
            std::shared_ptr<const DescriptorLayout> descriptor;
            vk::raii::PipelineLayout pipeline;

            Layouts(const vk::raii::Device &device, std::shared_ptr<const DescriptorLayout> descriptor,
                    std::span<const vk::PushConstantRange> ranges, vk::PipelineLayoutCreateFlags flags,
                    const void *next)
                : descriptor{std::move(descriptor)},
                  pipeline{device, vk::PipelineLayoutCreateInfo{
                      .pNext = next,
                      .flags = flags,
                      .setLayoutCount = 1,
                      .pSetLayouts = &*this->descriptor->layout,
                      .pushConstantRangeCount = static_cast<uint32_t>(ranges.size()),
                      .pPushConstantRanges = ranges.data(),
                  }} {}

            vk::DescriptorSetLayout GetDescriptorSetLayout() const {
                return *descriptor->layout;
            }
        };

      private:
        struct Binding {
            uint32_t binding;
            vk::DescriptorType type;
            uint32_t count;
            vk::ShaderStageFlags stages;

            bool operator==(const Binding &) const = default;
        };

        struct DescriptorKey {
            vk::DescriptorSetLayoutCreateFlags flags;
            std::vector<Binding> bindings;
            size_t hash;

            bool operator==(const DescriptorKey &) const = default;
        };

        struct DescriptorProbe {
            vk::DescriptorSetLayoutCreateFlags flags;
            std::span<const vk::DescriptorSetLayoutBinding> bindings;
            size_t hash;
        };

        struct PipelineKey {
            const DescriptorKey *descriptor; // unordered_map nodes survive rehash
            vk::PipelineLayoutCreateFlags flags;
            std::vector<vk::PushConstantRange> ranges;

            bool operator==(const PipelineKey &) const = default;
        };

        struct PipelineProbe {
            const DescriptorProbe &descriptor;
            vk::PipelineLayoutCreateFlags flags;
            std::span<const vk::PushConstantRange> ranges;
        };

        static void HashCombine(size_t &hash, size_t value) {
            hash ^= value + size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
        }

        static void HashBinding(size_t &hash, const vk::DescriptorSetLayoutBinding &binding) {
            HashCombine(hash, std::hash<uint64_t>{}((uint64_t{binding.binding} << 32) |
                                                  static_cast<uint32_t>(binding.descriptorType)));
            HashCombine(hash, std::hash<uint64_t>{}((uint64_t{binding.descriptorCount} << 32) |
                                                  static_cast<VkShaderStageFlags>(binding.stageFlags)));
        }

        static size_t HashBindings(vk::DescriptorSetLayoutCreateFlags flags,
                                   std::span<const vk::DescriptorSetLayoutBinding> bindings) {
            size_t hash{static_cast<VkDescriptorSetLayoutCreateFlags>(flags)};
            HashCombine(hash, bindings.size());
            for (const auto &binding : bindings)
                HashBinding(hash, binding);
            return hash;
        }

        struct DescriptorHash {
            using is_transparent = void;
            size_t operator()(const DescriptorKey &key) const { return key.hash; }
            size_t operator()(const DescriptorProbe &key) const { return key.hash; }
        };

        struct DescriptorEqual {
            using is_transparent = void;
            bool operator()(const DescriptorKey &a, const DescriptorKey &b) const { return a == b; }
            bool operator()(const DescriptorKey &a, const DescriptorProbe &b) const {
                if (a.flags != b.flags || a.bindings.size() != b.bindings.size())
                    return false;
                for (size_t i{}; i < a.bindings.size(); i++) {
                    const auto &x{a.bindings[i]};
                    const auto &y{b.bindings[i]};
                    if (x.binding != y.binding || x.type != y.descriptorType ||
                        x.count != y.descriptorCount || x.stages != y.stageFlags)
                        return false;
                }
                return true;
            }
            bool operator()(const DescriptorProbe &a, const DescriptorKey &b) const { return (*this)(b, a); }
        };

        struct PipelineHash {
            using is_transparent = void;
            static size_t Hash(size_t hash, vk::PipelineLayoutCreateFlags flags,
                               std::span<const vk::PushConstantRange> ranges) {
                HashCombine(hash, static_cast<VkPipelineLayoutCreateFlags>(flags));
                HashCombine(hash, ranges.size());
                for (const auto &range : ranges) {
                    HashCombine(hash, static_cast<VkShaderStageFlags>(range.stageFlags));
                    HashCombine(hash, std::hash<uint64_t>{}((uint64_t{range.offset} << 32) | range.size));
                }
                return hash;
            }
            size_t operator()(const PipelineKey &key) const { return Hash(key.descriptor->hash, key.flags, key.ranges); }
            size_t operator()(const PipelineProbe &key) const { return Hash(key.descriptor.hash, key.flags, key.ranges); }
        };

        struct PipelineEqual {
            using is_transparent = void;
            bool operator()(const PipelineKey &a, const PipelineKey &b) const { return a == b; }
            bool operator()(const PipelineKey &a, const PipelineProbe &b) const {
                return a.flags == b.flags && a.ranges.size() == b.ranges.size() &&
                       std::equal(a.ranges.begin(), a.ranges.end(), b.ranges.begin()) &&
                       DescriptorEqual{}(*a.descriptor, b.descriptor);
            }
            bool operator()(const PipelineProbe &a, const PipelineKey &b) const { return (*this)(b, a); }
        };

        const vk::raii::Device &device;
        std::mutex mutex;
        // Keep canonical handles until device teardown. DescriptorAllocator buckets
        // sets by handle; expiring/recreating layouts could recycle a bucket's key.
        // GPU declares this cache before that allocator, so it also outlives pools.
        std::unordered_map<DescriptorKey, std::shared_ptr<const DescriptorLayout>, DescriptorHash, DescriptorEqual> descriptors;
        std::unordered_map<PipelineKey, std::shared_ptr<const Layouts>, PipelineHash, PipelineEqual> pipelines;

      public:
        explicit PipelineLayoutCache(const vk::raii::Device &device) : device{device} {}

        std::shared_ptr<const Layouts> Acquire(const vk::DescriptorSetLayoutCreateInfo &info,
                                              std::span<const vk::PushConstantRange> ranges = {},
                                              vk::PipelineLayoutCreateFlags flags = {},
                                              const void *pipelineNext = nullptr) {
            auto uncached{[&] {
                return std::make_shared<Layouts>(device, std::make_shared<DescriptorLayout>(device, info), ranges, flags, pipelineNext);
            }};
            if (info.pNext || pipelineNext)
                return uncached();

            std::span<const vk::DescriptorSetLayoutBinding> bindings{info.pBindings, info.bindingCount};
            size_t hash{static_cast<VkDescriptorSetLayoutCreateFlags>(info.flags)};
            HashCombine(hash, bindings.size());
            bool ordered{true};
            for (size_t i{}; i < bindings.size(); i++) {
                const auto &binding{bindings[i]};
                if (binding.pImmutableSamplers)
                    return uncached();
                if (i && bindings[i - 1].binding >= binding.binding)
                    ordered = false;
                HashBinding(hash, binding);
            }

            std::vector<vk::DescriptorSetLayoutBinding> sorted;
            // Current builders already emit sorted bindings. A borrowed probe
            // avoids allocating/copying keys on hits; normalize other inputs.
            if (!ordered) {
                sorted.assign(bindings.begin(), bindings.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
                    return std::tie(a.binding, a.descriptorType, a.descriptorCount, a.stageFlags) <
                           std::tie(b.binding, b.descriptorType, b.descriptorCount, b.stageFlags);
                });
                bindings = sorted;
                hash = HashBindings(info.flags, bindings);
            }
            DescriptorProbe descriptorProbe{info.flags, bindings, hash};
            PipelineProbe probe{descriptorProbe, flags, ranges};
            std::scoped_lock lock{mutex};
            if (auto it{pipelines.find(probe)}; it != pipelines.end())
                return it->second;

            auto descriptorIt{descriptors.find(descriptorProbe)};
            if (descriptorIt == descriptors.end()) {
                DescriptorKey key{info.flags, {}, descriptorProbe.hash};
                key.bindings.reserve(bindings.size());
                for (const auto &binding : bindings)
                    key.bindings.push_back({binding.binding, binding.descriptorType, binding.descriptorCount, binding.stageFlags});
                auto descriptor{std::make_shared<DescriptorLayout>(device, info)};
                descriptorIt = descriptors.emplace(std::move(key), std::move(descriptor)).first;
            }

            PipelineKey key{&descriptorIt->first, flags, {ranges.begin(), ranges.end()}};
            auto layouts{std::make_shared<Layouts>(device, descriptorIt->second, ranges, flags, nullptr)};
            // Publish only fully-created objects. A failed pipeline-layout create
            // leaves at most its reusable descriptor layout, never a failed hit.
            pipelines.emplace(std::move(key), layouts);
            return layouts;
        }
    };
}
