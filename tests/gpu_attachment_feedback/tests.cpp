// SPDX-License-Identifier: MPL-2.0
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <gpu/attachment_feedback.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <functional>
#include <memory>
#include <span>
#include <optional>
#include <string>

using namespace skyline::gpu;
static void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Commands {
    std::vector<vk::ImageMemoryBarrier> barriers;
    VkCommandBuffer operator*() const { return reinterpret_cast<VkCommandBuffer>(uintptr_t{19}); }
    void pipelineBarrier(vk::PipelineStageFlags, vk::PipelineStageFlags, vk::DependencyFlags,
                         vk::ArrayProxy<const vk::MemoryBarrier>, vk::ArrayProxy<const vk::BufferMemoryBarrier>,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> images) {
        barriers.insert(barriers.end(), images.begin(), images.end());
    }
};
namespace vk::raii { using CommandBuffer = Commands; }
struct Texture {
    struct { uint32_t depth{1}; } dimensions;
    struct Format { vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor}; } value;
    Format *format{&value};
    vk::ImageUsageFlags usage{feedback::ImageUsageBit};
    vk::ImageLayout layout{vk::ImageLayout::eGeneral};
    vk::ImageView ownedView{reinterpret_cast<VkImageView>(uintptr_t{29})};
    bool OwnsView(vk::ImageView view) const { return view && view == ownedView; }
    uint32_t levelCount{3}, layerCount{2};
    vk::Image GetBacking() { return vk::Image{reinterpret_cast<VkImage>(uintptr_t{17})}; }
};
struct TextureView {
    std::shared_ptr<Texture> texture;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 3, 0, 2};
};
struct FenceCycle {};
static std::vector<VkImageAspectFlags> dynamicCalls;
static VKAPI_ATTR void VKAPI_CALL DynamicCommand(VkCommandBuffer commands, VkImageAspectFlags aspects) {
    Check(commands == reinterpret_cast<VkCommandBuffer>(uintptr_t{19}), "dynamic command buffer ABI changed");
    dynamicCalls.push_back(aspects);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetCommand(VkDevice, const char *name) {
    Check(std::string(name) == "vkCmdSetAttachmentFeedbackLoopEnableEXT", "dynamic command name changed");
    return reinterpret_cast<PFN_vkVoidFunction>(DynamicCommand);
}
struct Device {
    struct Dispatcher { PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr{GetCommand}; } dispatcher;
    const Dispatcher *getDispatcher() const { return &dispatcher; }
    VkDevice operator*() const { return reinterpret_cast<VkDevice>(uintptr_t{23}); }
};
struct GPU { struct { bool supportsAttachmentFeedbackLoop{true}; } traits; Device vkDevice; };
#include "dynamic_command.inc"

static void DynamicTests() {
    struct State {
        struct Context { GPU gpu; } ctx;
        std::optional<vk::ImageAspectFlags> activeAttachmentFeedbackAspects;
        struct Builder {
            GPU *gpu;
            void SetAttachmentFeedback(vk::ImageAspectFlags aspects) {
                Commands commands;
                SetAttachmentFeedbackCmdImpl{aspects}.Record(*gpu, commands);
            }
        } builder{&ctx.gpu};
        void Draw(vk::ImageAspectFlags feedbackAspects) {
#include "dynamic.inc"
        }
    } state;
    dynamicCalls.clear();
    state.Draw({});
    state.Draw({});
    state.Draw(vk::ImageAspectFlagBits::eColor);
    state.Draw(vk::ImageAspectFlagBits::eColor);
    state.Draw({});
    state.activeAttachmentFeedbackAspects.reset(); // New command buffer/pipeline invalidation.
    state.Draw({});
    state.ctx.gpu.traits.supportsAttachmentFeedbackLoop = false;
    state.activeAttachmentFeedbackAspects.reset();
    state.Draw({});
    Check(dynamicCalls == std::vector<VkImageAspectFlags>{0, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0},
          "dynamic feedback must initialize disabled state, toggle per draw and initialize new command buffers");
    std::cout << "PASS: production feedback dynamic state initialization, reuse, reset and extension ABI dispatch\n";
}
template<typename T> using span = std::span<T>;
using u32 = uint32_t;
using exception = std::runtime_error;
namespace node {
    struct NextSubpassFunctionNode {};
    struct SubpassFunctionNode {};
}
struct CommandExecutor {
    GPU gpu;
    struct Settings { std::optional<size_t> executorFlushThreshold{0}; } settings;
    struct State { Settings *settings; } state{&settings};
    std::vector<std::string> events;
    struct Nodes {
        CommandExecutor *owner;
        template<typename... Args> void emplace_back(Args &&...) { owner->events.push_back("draw"); }
        size_t size() { return owner->events.size(); }
    };
    struct Slot { Nodes nodes; } storage{{this}};
    Slot *slot{&storage};
    std::vector<std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)>> queued;
    using SubpassFunction = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)>;
    struct SubpassHooks { SubpassFunction before; };
    using SubpassHookFactory = std::function<SubpassHooks(u32)>;
    u32 renderPassIndex{};
    bool failCreate{};
    bool capturedFeedback{};
    bool submitted{};
    void AddOutsideRpCommand(decltype(queued)::value_type function) {
        events.push_back("outside"); queued.push_back(std::move(function));
    }
    void FinishRenderPass() { events.push_back("finish"); }
    void InsertPreRpCommand(decltype(queued)::value_type function) {
        events.insert(std::find(events.begin(), events.end(), "renderpass"), "outside");
        queued.push_back(std::move(function));
    }
    bool CreateRenderPassWithSubpass(vk::Rect2D, span<TextureView *>, span<TextureView *>, span<TextureView *> colors,
                                    TextureView *, bool, vk::PipelineStageFlags, vk::PipelineStageFlags) {
        events.push_back("renderpass");
        capturedFeedback = colors[0]->texture->layout == feedback::Layout;
        if (failCreate) throw std::runtime_error("renderpass error");
        return false;
    }
    void Submit() { events.push_back("submit"); submitted = true; }
    void AddSubpass(SubpassFunction &&, vk::Rect2D, span<TextureView *>, span<TextureView *>, span<TextureView *>,
                    TextureView *, bool, vk::PipelineStageFlags, vk::PipelineStageFlags, SubpassHookFactory, bool);
};
#include "executor.inc"

static void ExecutorTests() {
    auto texture{std::make_shared<Texture>()};
    TextureView sampled{texture}, color{texture};
    std::vector<TextureView *> sampledViews{&sampled}, colorViews{&color};
    CommandExecutor executor;
    executor.AddSubpass({}, {}, sampledViews, {}, colorViews, nullptr, false, {}, {}, {}, true);
    Check(executor.events == std::vector<std::string>{"finish", "outside", "renderpass", "draw", "outside", "submit"},
          "feedback transition/pass/draw/restore must precede threshold submit");
    Check(executor.capturedFeedback && texture->layout == vk::ImageLayout::eGeneral,
          "attachment capture or deferred upload metadata uses wrong layout");
    Commands commands;
    for (auto &record : executor.queued) record(commands, {}, executor.gpu);
    Check(commands.barriers.size() == 2 && commands.barriers.front().newLayout == feedback::Layout &&
          commands.barriers.back().newLayout == vk::ImageLayout::eGeneral, "queued feedback layouts are not paired");
    sampledViews.push_back(&sampled);
    Check(feedback::FindImages(sampledViews, colorViews, static_cast<TextureView *>(nullptr)).size() == 1,
          "multiple descriptors duplicate image transitions");
    vk::DescriptorImageInfo infos[2]{{.imageLayout = vk::ImageLayout::eGeneral}, {.imageLayout = vk::ImageLayout::eGeneral}};
    std::vector<vk::WriteDescriptorSet> writes{{.descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &infos[0]},
        {.descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &infos[1]}};
    const auto images{feedback::FindImages(sampledViews, colorViews, static_cast<TextureView *>(nullptr))};
    feedback::PatchDescriptorLayouts(writes, sampledViews, images);
    Check(infos[0].imageLayout == feedback::Layout && infos[1].imageLayout == feedback::Layout,
          "combined and samplerless descriptors disagree with attachment layout");
    vk::DescriptorImageInfo storageInfo{.imageView = texture->ownedView, .imageLayout = vk::ImageLayout::eGeneral};
    writes.push_back({.descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &storageInfo});
    bool rejectedStorage{};
    try { feedback::PatchDescriptorLayouts(writes, sampledViews, images); }
    catch (const std::runtime_error &) { rejectedStorage = true; }
    Check(rejectedStorage, "storage descriptor retained GENERAL on a feedback backing");
    storageInfo.imageView = vk::ImageView{reinterpret_cast<VkImageView>(uintptr_t{31})};
    feedback::PatchDescriptorLayouts(writes, sampledViews, images);
    for (bool allowed : {false, true}) {
        CommandExecutor unsupported;
        unsupported.gpu.traits.supportsAttachmentFeedbackLoop = !allowed;
        bool threw{};
        try { unsupported.AddSubpass({}, {}, sampledViews, {}, colorViews, nullptr, false, {}, {}, {}, allowed); }
        catch (const std::runtime_error &) { threw = true; }
        Check(threw && unsupported.events.empty(), "unsupported feedback published an invalid draw");
    }
    CommandExecutor failed;
    failed.failCreate = true;
    try { failed.AddSubpass({}, {}, sampledViews, {}, colorViews, nullptr, false, {}, {}, {}, true); }
    catch (const std::runtime_error &) {}
    Check(texture->layout == vk::ImageLayout::eGeneral && failed.queued.empty(),
          "failed pass creation leaked feedback metadata or an unpaired transition");
    sampled.texture = std::make_shared<Texture>();
    CommandExecutor ordinary;
    ordinary.AddSubpass({}, {}, sampledViews, {}, colorViews, nullptr, false, {}, {}, {}, true);
    Check(ordinary.queued.empty() && !ordinary.capturedFeedback, "ordinary draw incurred feedback transitions");
    std::cout << "PASS: production executor feedback lifetime, full descriptor layouts, failure and ordinary draw\n";
}
int main() {
    DynamicTests();
    ExecutorTests();
    const vk::ImageSubresourceRange first{vk::ImageAspectFlagBits::eColor, 1, 2, 2, 3};
    auto second{first};
    Check(feedback::RangesOverlap(first, second, false), "identical subresources must overlap");
    second.baseMipLevel = 3;
    Check(!feedback::RangesOverlap(first, second, false), "adjacent mip ranges must not overlap");
    second = first; second.baseArrayLayer = 5;
    Check(!feedback::RangesOverlap(first, second, false), "adjacent array layers must not overlap");
    Check(feedback::RangesOverlap(first, second, true), "3D slices share a Vulkan mip subresource");
    second = first; second.aspectMask = vk::ImageAspectFlagBits::eDepth;
    Check(!feedback::RangesOverlap(first, second, false), "separate aspects must not overlap");
    second = first; second.baseMipLevel = 0; second.levelCount = VK_REMAINING_MIP_LEVELS;
    Check(feedback::RangesOverlap(first, second, false), "remaining mip levels must overlap");
    Check(feedback::ImageUsage({}, true) == vk::ImageUsageFlags{}, "nonrenderable image gained feedback usage");
    const auto usage{vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment};
    Check(feedback::ImageUsage(usage, false) == usage, "unsupported device gained feedback usage");
    Check(bool(feedback::ImageUsage(usage, true) & feedback::ImageUsageBit), "renderable image missing feedback usage");
    Commands commands;
    const auto image{vk::Image{reinterpret_cast<VkImage>(uintptr_t{17})}};
    feedback::RecordLayoutTransition(commands, image, first, vk::ImageLayout::eGeneral, feedback::Layout);
    feedback::RecordLayoutTransition(commands, image, first, feedback::Layout, vk::ImageLayout::eGeneral);
    Check(commands.barriers.size() == 2, "feedback must enter and leave its layout");
    for (const auto &barrier : commands.barriers) {
        Check(barrier.image == image && barrier.subresourceRange == first, "barrier escaped the feedback image range");
        Check(barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
              "layout transition performs queue ownership transfer");
    }
    Check(commands.barriers[0].oldLayout == commands.barriers[1].newLayout &&
          commands.barriers[0].newLayout == commands.barriers[1].oldLayout, "feedback layouts are not restored");
    std::cout << "PASS: attachment feedback overlap, usage and image layout scope\n";
}
