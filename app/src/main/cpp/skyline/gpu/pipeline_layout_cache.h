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
#include <boost/container/small_vector.hpp>
#include <tsl/robin_set.h>
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
            explicit DescriptorLayout(std::nullptr_t) : layout{nullptr} {}
            DescriptorLayout(const vk::raii::Device &device, const vk::DescriptorSetLayoutCreateInfo &info)
                : layout{device, info} {}
        };

        struct Layouts {
          private:
            // A secondary pipeline interface owns its primary interface. Primary
            // interfaces contain the descriptor object inline; no ownership cycle.
            std::shared_ptr<const Layouts> descriptorOwner;
            DescriptorLayout ownedDescriptor;

            static vk::raii::PipelineLayout Create(const vk::raii::Device &device,
                    const DescriptorLayout *descriptor, std::span<const vk::PushConstantRange> ranges,
                    vk::PipelineLayoutCreateFlags flags, const void *next) {
                return vk::raii::PipelineLayout{device, vk::PipelineLayoutCreateInfo{
                    .pNext = next,
                    .flags = flags,
                    .setLayoutCount = 1,
                    .pSetLayouts = &*descriptor->layout,
                    .pushConstantRangeCount = static_cast<uint32_t>(ranges.size()),
                    .pPushConstantRanges = ranges.data(),
                }};
            }

          public:
            const DescriptorLayout *const descriptor;
            vk::raii::PipelineLayout pipeline;

            Layouts(const vk::raii::Device &device, const vk::DescriptorSetLayoutCreateInfo &info,
                    std::span<const vk::PushConstantRange> ranges, vk::PipelineLayoutCreateFlags flags,
                    const void *next)
                : ownedDescriptor{device, info}, descriptor{&ownedDescriptor},
                  pipeline{Create(device, descriptor, ranges, flags, next)} {}

            Layouts(const vk::raii::Device &device, std::shared_ptr<const Layouts> primary,
                    std::span<const vk::PushConstantRange> ranges, vk::PipelineLayoutCreateFlags flags)
                : descriptorOwner{std::move(primary)}, ownedDescriptor{nullptr},
                  descriptor{descriptorOwner->descriptor}, pipeline{Create(device, descriptor, ranges, flags, nullptr)} {}

            Layouts(const Layouts &) = delete;
            Layouts(Layouts &&) = delete;
            Layouts &operator=(const Layouts &) = delete;
            Layouts &operator=(Layouts &&) = delete;

            vk::DescriptorSetLayout GetDescriptorSetLayout() const { return *descriptor->layout; }
        };

      private:
        struct Binding {
            uint32_t binding;
            vk::DescriptorType type;
            uint32_t count;
            vk::ShaderStageFlags stages;

            bool operator==(const Binding &) const = default;
        };

        struct DescriptorProbe {
            vk::DescriptorSetLayoutCreateFlags flags;
            std::span<const vk::DescriptorSetLayoutBinding> bindings;
            size_t hash;
        };

        struct DescriptorKey {
            vk::DescriptorSetLayoutCreateFlags flags;
            boost::container::small_vector<Binding, 4> bindings;
            size_t hash;

            explicit DescriptorKey(const DescriptorProbe &probe) : flags{probe.flags}, hash{probe.hash} {
                bindings.reserve(probe.bindings.size());
                for (const auto &binding : probe.bindings)
                    bindings.push_back({binding.binding, binding.descriptorType, binding.descriptorCount, binding.stageFlags});
            }
            bool operator==(const DescriptorKey &) const = default;
        };

        // The first pipeline interface and its descriptor share one allocation
        // and control block. Aliasing shared_ptrs keep these immovable members
        // alive across table growth and asynchronous compilation.
        struct Entry {
            DescriptorKey key;
            vk::PipelineLayoutCreateFlags flags;
            boost::container::small_vector<vk::PushConstantRange, 1> ranges;
            Layouts primary;

            Entry(const vk::raii::Device &device, const DescriptorProbe &probe,
                  const vk::DescriptorSetLayoutCreateInfo &info,
                  std::span<const vk::PushConstantRange> ranges, vk::PipelineLayoutCreateFlags flags)
                : key{probe}, flags{flags}, ranges{ranges.begin(), ranges.end()},
                  primary{device, info, ranges, flags, nullptr} {}
        };

        struct PipelineKey {
            const Entry *descriptor;
            vk::PipelineLayoutCreateFlags flags;
            boost::container::small_vector<vk::PushConstantRange, 1> ranges;
            bool operator==(const PipelineKey &) const = default;
        };

        struct PipelineProbe {
            const Entry *descriptor;
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

        struct EntryHash {
            size_t operator()(const std::shared_ptr<Entry> &entry) const { return entry->key.hash; }
            size_t operator()(const DescriptorProbe &probe) const { return probe.hash; }
        };

        struct EntryEqual {
            using is_transparent = void;
            bool operator()(const std::shared_ptr<Entry> &a, const std::shared_ptr<Entry> &b) const {
                return DescriptorEqual{}(a->key, b->key);
            }
            bool operator()(const std::shared_ptr<Entry> &a, const DescriptorProbe &b) const {
                return DescriptorEqual{}(a->key, b);
            }
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
            size_t operator()(const PipelineKey &key) const { return Hash(key.descriptor->key.hash, key.flags, key.ranges); }
            size_t operator()(const PipelineProbe &key) const { return Hash(key.descriptor->key.hash, key.flags, key.ranges); }
        };

        struct PipelineEqual {
            using is_transparent = void;
            bool operator()(const PipelineKey &a, const PipelineKey &b) const { return a == b; }
            bool operator()(const PipelineKey &a, const PipelineProbe &b) const {
                return a.flags == b.flags && a.ranges.size() == b.ranges.size() &&
                       std::equal(a.ranges.begin(), a.ranges.end(), b.ranges.begin()) &&
                       a.descriptor == b.descriptor;
            }
            bool operator()(const PipelineProbe &a, const PipelineKey &b) const { return (*this)(b, a); }
        };

        const vk::raii::Device &device;
        std::mutex mutex;
        // Strong owners keep descriptor handles stable for DescriptorAllocator.
        // Rehash moves shared_ptrs, never the Entry or its inline Vulkan objects.
        // Prime growth avoids clustering from structured binding/count high bits.
        // No eviction: this GPU member outlives DescriptorAllocator's handle-keyed
        // pools. Destruction requires all Acquire callers to have stopped, with
        // the logical device alive for every exported owner (as for other RAII).
        tsl::robin_pg_set<std::shared_ptr<Entry>, EntryHash, EntryEqual,
                          std::allocator<std::shared_ptr<Entry>>, true> entries;
        // Only additional push-constant/flag interfaces need this second index.
        std::unordered_map<PipelineKey, std::shared_ptr<const Layouts>, PipelineHash, PipelineEqual> variants;

      public:
        explicit PipelineLayoutCache(const vk::raii::Device &device) : device{device} {}

        std::shared_ptr<const Layouts> Acquire(const vk::DescriptorSetLayoutCreateInfo &info,
                                              std::span<const vk::PushConstantRange> ranges = {},
                                              vk::PipelineLayoutCreateFlags flags = {},
                                              const void *pipelineNext = nullptr) {
            auto uncached{[&] {
                return std::make_shared<Layouts>(device, info, ranges, flags, pipelineNext);
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
            std::scoped_lock lock{mutex};
            if (auto it{entries.find(descriptorProbe)}; it != entries.end()) {
                const auto &entry{*it};
                auto primary{std::shared_ptr<const Layouts>{entry, &entry->primary}};
                if (entry->flags == flags && entry->ranges.size() == ranges.size() &&
                    std::equal(entry->ranges.begin(), entry->ranges.end(), ranges.begin()))
                    return primary;
                PipelineProbe probe{entry.get(), flags, ranges};
                if (auto variant{variants.find(probe)}; variant != variants.end())
                    return variant->second;
                PipelineKey key{entry.get(), flags, {ranges.begin(), ranges.end()}};
                auto layouts{std::make_shared<Layouts>(device, std::move(primary), ranges, flags)};
                variants.emplace(std::move(key), layouts);
                return layouts;
            }

            auto entry{std::make_shared<Entry>(device, descriptorProbe, info, ranges, flags)};
            // Publish only a complete descriptor/pipeline pair. A Vulkan create
            // or table-allocation failure destroys the unpublished owner; retry
            // cannot hit a partial entry. The mutex also serializes first use.
            entries.insert(entry);
            return std::shared_ptr<const Layouts>{entry, &entry->primary};
        }
    };
}
