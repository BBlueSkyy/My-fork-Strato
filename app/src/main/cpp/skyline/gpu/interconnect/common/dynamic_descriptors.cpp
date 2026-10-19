// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <limits>
#include <stdexcept>
#include <gpu.h>
#include <gpu/interconnect/command_executor.h>
#include "dynamic_descriptors.h"
#include "textures.h"
#include "samplers.h"

namespace skyline::gpu::interconnect::dynamic_descriptor_detail {
    enum class SnapshotAction { Reuse, ResetAndReuse, Clone };
    inline uint32_t Capacity(uint32_t count) {
        uint32_t result{1};
        while (result < count) {
            if (result > std::numeric_limits<uint32_t>::max() / 2)
                throw std::overflow_error("Dynamic descriptor capacity overflow");
            result *= 2;
        }
        return result;
    }
    inline bool Fits(const std::vector<uint32_t> &counts, const std::vector<uint32_t> &capacities) {
        if (counts.size() != capacities.size()) return false;
        for (size_t i{}; i < counts.size(); ++i) if (counts[i] > capacities[i]) return false;
        return true;
    }
    template<class T> auto ChangedRanges(const std::vector<T> &before, const std::vector<T> &after) {
        std::vector<std::pair<uint32_t, uint32_t>> result;
        for (uint32_t i{}; i < after.size();) {
            if (i < before.size() && before[i] == after[i]) { ++i; continue; }
            const auto first{i++};
            while (i < after.size() && (i >= before.size() || before[i] != after[i])) ++i;
            result.emplace_back(first, i - first);
        }
        return result;
    }
    inline SnapshotAction ChooseSnapshot(bool sameContent, bool sameCycle, bool completed) {
        if (!sameContent) return SnapshotAction::Clone;
        if (sameCycle) return SnapshotAction::Reuse;
        return completed ? SnapshotAction::ResetAndReuse : SnapshotAction::Clone;
    }
    inline uint32_t Encode(uint32_t index, uint32_t type) { return (index << 2) | type; }
    inline uint32_t SamplerIndex(uint32_t handle, bool viaHeader) { return viaHeader ? handle & 0xFFFFF : handle >> 20; }
}
// End independently testable descriptor policy.

namespace skyline::gpu::interconnect {
    using namespace dynamic_descriptor_detail;
    struct DynamicDescriptors::Impl {
        static constexpr u32 Invalid{~u32{}};
        struct ImageEntry {
            vk::ImageView view;
            u64 backingGeneration{};
            const Texture *texture{};
            bool operator==(const ImageEntry &) const = default;
            explicit operator bool() const { return static_cast<bool>(view); }
        };
        struct Candidate { u32 index; u32 numeric; std::shared_ptr<TextureView> view; };
        struct State {
            std::vector<u32> words;
            std::vector<std::vector<Candidate>> candidates;
            std::vector<std::vector<ImageEntry>> images;
            std::vector<vk::Sampler> samplers;
            std::vector<std::shared_ptr<vk::raii::Sampler>> retainedSamplers;
            std::vector<u32> Counts() const {
                std::vector<u32> result{static_cast<u32>(samplers.size())};
                for (const auto &array : images) result.push_back(static_cast<u32>(array.size()));
                return result;
            }
        };
        struct Snapshot {
            vk::raii::DescriptorPool pool{nullptr};
            vk::DescriptorSet set;
            std::unique_ptr<memory::Buffer> metadata;
            State state;
            std::weak_ptr<FenceCycle> cycle;
        };
        struct FaultReporter {
            std::shared_ptr<Snapshot> snapshot;
            ~FaultReporter() {
                auto words{reinterpret_cast<const u32 *>(snapshot->metadata->data())};
                if (words[3])
                    LOGE("Dynamic texture descriptor access fault: code={} handle=0x{:08X}", words[3], words[4]);
            }
        };
        std::vector<Shader::DynamicTextureDescriptor> groups;
        std::vector<u32> capacities;
        vk::raii::DescriptorSetLayout layout{nullptr};
        std::shared_ptr<Snapshot> current;

        State Collect(InterconnectContext &ctx, Textures &textures, Samplers &samplers, bool prepare,
                      vk::PipelineStageFlags &src, vk::PipelineStageFlags &dst) const {
            const auto headers{textures.GetPoolHeaders(ctx)};
            const auto samplerHeaders{samplers.GetPoolHeaders(ctx)};
            if (headers.size() > 0x100000 || samplerHeaders.size() > 0x100000)
                throw exception("Dynamic descriptor guest pool exceeds handle index range");
            State result;
            result.samplers.resize(samplerHeaders.size());
            result.candidates.resize(groups.size());
            result.images.resize(groups.size() * 3);
            result.words.resize(8 + groups.size() + samplerHeaders.size(), 0);
            result.words[0] = headers.size(); result.words[1] = samplerHeaders.size();
            result.words[2] = samplers.IsViaHeaderBinding(); result.words[5] = groups.size();
            for (u32 index{}; index < samplerHeaders.size(); ++index) {
                auto sampler{samplers.GetDynamicSampler(ctx, index)};
                if (sampler) {
                    result.samplers[index] = **sampler;
                    result.retainedSamplers.push_back(std::move(sampler));
                    result.words[8 + groups.size() + index] = 1 | (samplers.DynamicSamplerUsesLinear(index) ? 2u : 0u);
                }
            }
            for (u32 group{}; group < groups.size(); ++group) {
                const u32 map{static_cast<u32>(result.words.size())};
                result.words[8 + group] = map;
                result.words.resize(result.words.size() + headers.size(), Invalid);
                for (u32 index{}; index < headers.size(); ++index) {
                    auto view{textures.GetDynamicTexture(ctx, index, groups[group].type)};
                    if (!view) continue;
                    const auto component{textures.GetDynamicTextureComponentType(index)};
                    const u32 numeric{component == Shader::TextureComponentType::Uint ? 1u : component == Shader::TextureComponentType::Sint ? 2u : 0u};
                    auto &images{result.images[3 * group + numeric]};
                    if (images.size() >= (u32{1} << 29))
                        throw exception("Dynamic texture compact index exceeds metadata encoding");
                    result.words[map + index] = Encode(images.size(), numeric) |
                        (textures.DynamicTextureSupportsLinear(ctx, index) ? 0x80000000u : 0u);
                    result.candidates[group].push_back({index, numeric, view->shared_from_this()});
                    if (prepare) {
                        ctx.executor.AttachTexture(view);
                        if (!(view->GetUsage() & vk::ImageUsageFlagBits::eSampled))
                            throw exception("Dynamic texture view does not support sampled image access");
                        // TransitionLayout waits on its prior fence even for a
                        // no-op. Never serialize unchanged descriptor candidates.
                        if (view->texture->layout != vk::ImageLayout::eGeneral)
                            view->texture->TransitionLayout(vk::ImageLayout::eGeneral);
                        view->texture->PopulateReadBarrier(vk::PipelineStageFlagBits::eComputeShader, src, dst);
                        const auto handle{view->GetView()};
                        images.push_back({handle, view->texture->GetBackingGeneration(), view->texture.get()});
                    } else images.emplace_back();
                }
            }
            if (result.words.size() * sizeof(u32) > ctx.gpu.traits.maximumStorageBufferRange)
                throw exception("Dynamic texture metadata exceeds maximum storage buffer range");
            return result;
        }
        std::shared_ptr<Snapshot> Create(InterconnectContext &ctx, State state) {
            auto snapshot{std::make_shared<Snapshot>()};
            u32 imageCount{};
            for (size_t i{1}; i < capacities.size(); ++i) imageCount += capacities[i];
            const std::array sizes{
                vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampler, .descriptorCount = capacities[0]},
                vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampledImage, .descriptorCount = imageCount},
                vk::DescriptorPoolSize{.type = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1},
            };
            snapshot->pool = vk::raii::DescriptorPool{ctx.gpu.vkDevice, vk::DescriptorPoolCreateInfo{
                .maxSets = 1, .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()}};
            const vk::DescriptorSetLayout setLayout{*layout};
            const vk::DescriptorSetAllocateInfo allocateInfo{
                .descriptorPool = *snapshot->pool, .descriptorSetCount = 1, .pSetLayouts = &setLayout};
            const auto allocated{(*ctx.gpu.vkDevice).allocateDescriptorSets(&allocateInfo, &snapshot->set, *ctx.gpu.vkDevice.getDispatcher())};
            if (allocated != vk::Result::eSuccess)
                throw exception("Dynamic descriptor set allocation failed: {}", vk::to_string(allocated));
            snapshot->metadata = std::make_unique<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(state.words.size() * sizeof(u32)));
            std::memcpy(snapshot->metadata->data(), state.words.data(), state.words.size() * sizeof(u32));
            const vk::DescriptorBufferInfo metadata{.buffer = snapshot->metadata->vkBuffer, .offset = 0, .range = state.words.size() * sizeof(u32)};
            ctx.gpu.vkDevice.updateDescriptorSets(vk::WriteDescriptorSet{
                .dstSet = snapshot->set, .dstBinding = 1, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &metadata}, {});
            // Copy only initialized entries. Unused slots stay partially bound.
            auto update{[&](u32 binding, vk::DescriptorType type, const auto &before, const auto &after) {
                // Copy unchanged, initialized runs only; invalid sampler holes must
                // never become descriptor-copy sources.
                if (current) for (u32 i{}; i < std::min(before.size(), after.size());) {
                    if (!before[i] || before[i] != after[i]) { ++i; continue; }
                    const u32 first{i++};
                    while (i < std::min(before.size(), after.size()) && before[i] && before[i] == after[i]) ++i;
                    ctx.gpu.vkDevice.updateDescriptorSets({}, vk::CopyDescriptorSet{
                        .srcSet = current->set, .srcBinding = binding, .srcArrayElement = first,
                        .dstSet = snapshot->set, .dstBinding = binding, .dstArrayElement = first,
                        .descriptorCount = i - first});
                }
                for (const auto &[offset, count] : ChangedRanges(before, after)) {
                    // Invalid samplers are never accessed and are not copied/written.
                    for (u32 i{offset}; i < offset + count; ++i) {
                        if (!after[i]) continue;
                        vk::DescriptorImageInfo info{};
                        if constexpr (std::is_same_v<typename std::decay_t<decltype(after)>::value_type, vk::Sampler>) info.sampler = after[i];
                        else { info.imageView = after[i].view; info.imageLayout = vk::ImageLayout::eGeneral; }
                        ctx.gpu.vkDevice.updateDescriptorSets(vk::WriteDescriptorSet{
                            .dstSet = snapshot->set, .dstBinding = binding, .dstArrayElement = i,
                            .descriptorCount = 1, .descriptorType = type, .pImageInfo = &info}, {});
                    }
                }
            }};
            const std::vector<vk::Sampler> noSamplers;
            const std::vector<ImageEntry> noImages;
            update(0, vk::DescriptorType::eSampler, current ? current->state.samplers : noSamplers, state.samplers);
            for (u32 i{}; i < state.images.size(); ++i)
                update(2 + i, vk::DescriptorType::eSampledImage, current ? current->state.images[i] : noImages, state.images[i]);
            snapshot->state = std::move(state);
            return snapshot;
        }
    };

    DynamicDescriptors::DynamicDescriptors(InterconnectContext &ctx, Textures &textures, Samplers &samplers,
                                           span<const Shader::DynamicTextureDescriptor> groups) : impl{std::make_unique<Impl>()} {
        if (groups.empty()) throw exception("Dynamic descriptor table requires at least one texture group");
        impl->groups.assign(groups.begin(), groups.end());
        for (const auto &group : groups)
            if (group.type != Shader::TextureType::Color2D || group.is_depth)
                throw exception("Dynamic texture descriptors initially support non-depth 2D images only");
        vk::PipelineStageFlags src{}, dst{};
        const auto state{impl->Collect(ctx, textures, samplers, false, src, dst)};
        for (auto count : state.Counts()) impl->capacities.push_back(Capacity(count));
        ValidateLimits(ctx, 0, 0, 0);
        std::vector<vk::DescriptorSetLayoutBinding> bindings{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampler, .descriptorCount = impl->capacities[0], .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        };
        for (u32 i{1}; i < impl->capacities.size(); ++i)
            bindings.push_back({.binding = i + 1, .descriptorType = vk::DescriptorType::eSampledImage,
                               .descriptorCount = impl->capacities[i], .stageFlags = vk::ShaderStageFlagBits::eCompute});
        std::vector<vk::DescriptorBindingFlags> flags(bindings.size(), vk::DescriptorBindingFlagBits::ePartiallyBound);
        flags[1] = {};
        const vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{.bindingCount = static_cast<u32>(flags.size()), .pBindingFlags = flags.data()};
        impl->layout = vk::raii::DescriptorSetLayout{ctx.gpu.vkDevice, vk::DescriptorSetLayoutCreateInfo{
            .pNext = &bindingFlags, .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()}};
    }
    DynamicDescriptors::~DynamicDescriptors() = default;
    vk::DescriptorSetLayout DynamicDescriptors::Layout() const { return *impl->layout; }
    void DynamicDescriptors::ValidateLimits(InterconnectContext &ctx, u32 staticSampled, u32 staticSamplers, u32 staticResources, u32 staticStorage) const {
        const auto &limits{ctx.gpu.traits.dynamicDescriptorLimits};
        u64 images{};
        for (size_t i{1}; i < impl->capacities.size(); ++i) images += impl->capacities[i];
        const u64 samplers{impl->capacities[0]};
        if (images + staticSampled > limits.maxPerStageDescriptorSampledImages || images + staticSampled > limits.maxDescriptorSetSampledImages ||
            samplers + staticSamplers > limits.maxPerStageDescriptorSamplers || samplers + staticSamplers > limits.maxDescriptorSetSamplers ||
            images + samplers + 1 + staticResources > limits.maxPerStageResources || limits.maxBoundDescriptorSets < 2 ||
            u64{staticStorage} + 1 > limits.maxPerStageDescriptorStorageBuffers ||
            u64{staticStorage} + 1 > limits.maxDescriptorSetStorageBuffers)
            throw exception("Dynamic texture descriptor tables exceed host descriptor limits");
    }
    bool DynamicDescriptors::CanRepresent(InterconnectContext &ctx, Textures &textures, Samplers &samplers) {
        vk::PipelineStageFlags src{}, dst{};
        return Fits(impl->Collect(ctx, textures, samplers, false, src, dst).Counts(), impl->capacities);
    }
    vk::DescriptorSet DynamicDescriptors::Sync(InterconnectContext &ctx, Textures &textures, Samplers &samplers,
                                              vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto state{impl->Collect(ctx, textures, samplers, true, srcStageMask, dstStageMask)};
        if (!Fits(state.Counts(), impl->capacities)) throw exception("Dynamic descriptor layout requires a larger pipeline variant");
        auto previous{impl->current};
        auto oldCycle{previous ? previous->cycle.lock() : nullptr};
        const bool sameCycle{oldCycle == ctx.executor.cycle};
        const bool same{previous && previous->state.words == state.words && previous->state.images == state.images && previous->state.samplers == state.samplers};
        const bool completed{oldCycle && !sameCycle && oldCycle->Poll(false, true)};
        const auto action{ChooseSnapshot(same, sameCycle, completed || !oldCycle)};
        if (action == SnapshotAction::Clone) impl->current = impl->Create(ctx, std::move(state));
        else if (action == SnapshotAction::ResetAndReuse) {
            auto words{reinterpret_cast<u32 *>(previous->metadata->data())};
            words[3] = words[4] = 0;
        }
        auto snapshot{impl->current};
        if (action != SnapshotAction::Reuse) {
            snapshot->cycle = ctx.executor.cycle;
            auto reporter{std::make_shared<Impl::FaultReporter>()};
            reporter->snapshot = snapshot;
            ctx.executor.AttachDependency(reporter);
        }
        ctx.executor.AttachDependency(snapshot);
        return snapshot->set;
    }
}
