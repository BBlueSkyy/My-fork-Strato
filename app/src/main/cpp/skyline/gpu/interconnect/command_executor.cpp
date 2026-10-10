// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <range/v3/view.hpp>
#include <adrenotools/driver.h>
#include <common/settings.h>
#include <loader/loader.h>
#include <gpu.h>
#include <gpu/texture/storage.h>
#include <dlfcn.h>
#include "command_executor.h"
#include <nce.h>

namespace skyline::gpu::interconnect {
    namespace {
        std::atomic_uint64_t depthSliceCopyId{};

        template<typename Handle>
        std::uint64_t HandleValue(Handle handle) {
            if constexpr (std::is_pointer_v<Handle>)
                return reinterpret_cast<std::uintptr_t>(handle);
            else
                return static_cast<std::uint64_t>(handle);
        }

        std::uint64_t ImageHandleValue(vk::Image image) {
            return HandleValue(static_cast<VkImage>(image));
        }

        const char *ImageKindName(texture::ImageKind kind) {
            switch (kind) {
                case texture::ImageKind::OneDimensional: return "1D";
                case texture::ImageKind::TwoDimensional: return "2D";
                case texture::ImageKind::ThreeDimensional: return "3D";
            }
            return "?";
        }

        const char *CopyStateName(
            const std::optional<texture::CopyRepresentationDebugInfo> &info) {
            if (!info)
                return "Untracked";
            switch (info->state) {
                case texture::CopyRepresentationState::Untracked: return "Untracked";
                case texture::CopyRepresentationState::Current: return "Current";
                case texture::CopyRepresentationState::Stale: return "Stale";
            }
            return "?";
        }

        const char *TextureImageTypeName(const Texture &image) {
            if (!image.guest)
                return "?";
            switch (image.guest->GetImageType()) {
                case vk::ImageType::e1D: return "1D";
                case vk::ImageType::e2D: return "2D";
                case vk::ImageType::e3D: return "3D";
            }
            return "?";
        }

        texture::Dimensions ActualMipDimensions(const Texture &image, std::uint32_t mip) {
            if (mip >= image.levelCount || mip >= image.mipLayouts.size())
                return {};
            return image.mipLayouts[mip].dimensions;
        }

        bool IsDepthSliceCopy(const texture::ExactImageCopyRegion &region) {
            return (region.sourceImageType == texture::ImageKind::ThreeDimensional &&
                    region.destinationImageType == texture::ImageKind::TwoDimensional) ||
                (region.sourceImageType == texture::ImageKind::TwoDimensional &&
                 region.destinationImageType == texture::ImageKind::ThreeDimensional);
        }

        vk::ImageType VkImageTypeOf(texture::ImageKind kind) {
            switch (kind) {
                case texture::ImageKind::OneDimensional: return vk::ImageType::e1D;
                case texture::ImageKind::TwoDimensional: return vk::ImageType::e2D;
                case texture::ImageKind::ThreeDimensional: return vk::ImageType::e3D;
            }
            return vk::ImageType::e2D;
        }

        const char *ValidateDepthSliceCopy(
            const std::shared_ptr<texture::TextureStorage> &sourceStorage,
            Texture &sourceTexture, vk::Image sourceImage,
            const std::shared_ptr<texture::TextureStorage> &destinationStorage,
            Texture &destinationTexture, vk::Image destinationImage,
            const texture::PreparedCopySynchronization<texture::TextureStorage> &prepared) {
            const auto &region{prepared.copyRegion};
            if (!IsDepthSliceCopy(region))
                return "not a 3D/2D depth-slice pair";
            if (!sourceStorage || !destinationStorage || sourceStorage == destinationStorage)
                return "source/destination storage is missing or identical";
            if (!sourceImage || !destinationImage || sourceImage == destinationImage)
                return "source/destination VkImage is missing or identical";
            if (sourceTexture.GetBacking() != sourceImage ||
                destinationTexture.GetBacking() != destinationImage)
                return "captured VkImage no longer matches its texture";
            if (!sourceTexture.guest || !destinationTexture.guest ||
                sourceTexture.guest->GetImageType() != VkImageTypeOf(region.sourceImageType) ||
                destinationTexture.guest->GetImageType() !=
                    VkImageTypeOf(region.destinationImageType))
                return "route imageType does not match the real VkImage type";
            if (prepared.read.sourceSubresource != region.sourceSubresource ||
                prepared.read.destinationSubresource != region.destinationSubresource)
                return "prepared subresource does not match the registered route";
            if (region.sourceSubresource.mip >= sourceTexture.levelCount ||
                region.sourceSubresource.mip >= sourceTexture.mipLayouts.size() ||
                region.destinationSubresource.mip >= destinationTexture.levelCount ||
                region.destinationSubresource.mip >= destinationTexture.mipLayouts.size())
                return "mip is outside the real image";

            const auto sourceMip{
                ActualMipDimensions(sourceTexture, region.sourceSubresource.mip)};
            const auto destinationMip{
                ActualMipDimensions(destinationTexture, region.destinationSubresource.mip)};
            if (!region.width || !region.height || region.depth != 1 ||
                region.width > sourceMip.width || region.height > sourceMip.height ||
                region.width > destinationMip.width || region.height > destinationMip.height)
                return "copy extent is outside a real mip";

            const auto validateSide = [&](const Texture &image, texture::ImageKind kind,
                                          texture::ResolvedSubresource subresource,
                                          std::uint32_t offsetZ,
                                          texture::Dimensions mip) -> const char * {
                if (kind == texture::ImageKind::ThreeDimensional) {
                    if (subresource.layer != 0 || image.layerCount != 1)
                        return "3D subresource has a nonzero layer or array layers";
                    if (subresource.depthSlice >= mip.depth ||
                        offsetZ != subresource.depthSlice ||
                        offsetZ > mip.depth || region.depth > mip.depth - offsetZ)
                        return "3D depthSlice/offset.z is outside mip depth";
                } else if (kind == texture::ImageKind::TwoDimensional) {
                    if (subresource.depthSlice != 0 || offsetZ != 0 || region.depth != 1 ||
                        mip.depth != 1 || subresource.layer >= image.layerCount)
                        return "2D subresource/layer/z/depth is invalid";
                } else {
                    return "unexpected image type in depth-slice route";
                }
                return nullptr;
            };
            if (const auto reason = validateSide(
                    sourceTexture, region.sourceImageType, region.sourceSubresource,
                    region.sourceOffsetZ, sourceMip))
                return reason;
            if (const auto reason = validateSide(
                    destinationTexture, region.destinationImageType,
                    region.destinationSubresource, region.destinationOffsetZ, destinationMip))
                return reason;
            if (sourceTexture.format->vkFormat != destinationTexture.format->vkFormat ||
                sourceTexture.sampleCount != destinationTexture.sampleCount ||
                sourceTexture.sampleCount != vk::SampleCountFlagBits::e1)
                return "format or sample count is incompatible";
            const auto aspect{vk::ImageAspectFlags{region.aspectMask}};
            if (!region.aspectMask ||
                (sourceTexture.format->vkAspect & aspect) != aspect ||
                (destinationTexture.format->vkAspect & aspect) != aspect)
                return "copy aspect is incompatible with an image";
            if (!(sourceTexture.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
                !(destinationTexture.usage & vk::ImageUsageFlagBits::eTransferDst))
                return "VkImage transfer usage is missing";
            if (sourceTexture.layout != vk::ImageLayout::eGeneral ||
                destinationTexture.layout != vk::ImageLayout::eGeneral)
                return "VkImage is not in GENERAL layout";
            return nullptr;
        }
    }

    static void RecordFullBarrier(vk::raii::CommandBuffer &commandBuffer) {
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            }, {}, {}
        );
    }

    CommandRecordThread::CommandRecordThread(const DeviceState &state)
        : state{state},
          incoming{1U << *state.settings->executorSlotCountScale},
          outgoing{1U << *state.settings->executorSlotCountScale},
          thread{&CommandRecordThread::Run, this} {}

    CommandRecordThread::~CommandRecordThread() {
        Stop();
    }

    void CommandRecordThread::Stop() {
        if (thread.joinable()) {
            incoming.Close();
            thread.join();
        }
    }

    CommandRecordThread::Slot::ScopedBegin::ScopedBegin(CommandRecordThread::Slot &slot) : slot{slot} {}

    CommandRecordThread::Slot::ScopedBegin::~ScopedBegin() {
        slot.Begin();
    }

    static vk::raii::CommandBuffer AllocateRaiiCommandBuffer(GPU &gpu, vk::raii::CommandPool &pool) {
        return {gpu.vkDevice, (*gpu.vkDevice).allocateCommandBuffers(
                    {
                        .commandPool = *pool,
                        .level = vk::CommandBufferLevel::ePrimary,
                        .commandBufferCount = 1
                    }, *gpu.vkDevice.getDispatcher()).front(),
                *pool};
    }

    CommandRecordThread::Slot::Slot(GPU &gpu)
        : commandPool{gpu.vkDevice,
                      vk::CommandPoolCreateInfo{
                          .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
                          .queueFamilyIndex = gpu.vkQueueFamilyIndex
                      }
          },
          commandBuffer{AllocateRaiiCommandBuffer(gpu, commandPool)},
          fence{gpu.vkDevice, vk::FenceCreateInfo{ .flags = vk::FenceCreateFlagBits::eSignaled }},
          semaphore{gpu.vkDevice, vk::SemaphoreCreateInfo{}},
          cycle{std::make_shared<FenceCycle>(gpu.vkDevice, *fence, *semaphore, true)},
          nodes{allocator},
          pendingPostRenderPassNodes{allocator} {
        Begin();
    }

    CommandRecordThread::Slot::Slot(Slot &&other)
        : commandPool{std::move(other.commandPool)},
          commandBuffer{std::move(other.commandBuffer)},
          fence{std::move(other.fence)},
          semaphore{std::move(other.semaphore)},
          cycle{std::move(other.cycle)},
          allocator{std::move(other.allocator)},
          nodes{std::move(other.nodes)},
          pendingPostRenderPassNodes{std::move(other.pendingPostRenderPassNodes)},
          ready{other.ready} {}

    std::shared_ptr<FenceCycle> CommandRecordThread::Slot::Reset(GPU &gpu) {
        auto startTime{util::GetTimeNs()};

        cycle->Wait();
        cycle = std::make_shared<FenceCycle>(*cycle);
        if (util::GetTimeNs() - startTime > GrowThresholdNs)
            didWait = true;

        // Command buffer doesn't need to be reset since that's done implicitly by begin
        return cycle;
    }

    void CommandRecordThread::Slot::WaitReady() {
        std::unique_lock lock{beginLock};
        beginCondition.wait(lock, [this] { return ready; });
        cycle->AttachObject(std::make_shared<ScopedBegin>(*this));
    }

    void CommandRecordThread::Slot::Begin() {
        std::unique_lock lock{beginLock};
        commandBuffer.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        });
        ready = true;
        beginCondition.notify_all();
    }

    void CommandRecordThread::ProcessSlot(Slot *slot) {
        TRACE_EVENT_FMT("gpu", "ProcessSlot: {}, execution: {}", fmt::ptr(slot), u64{slot->executionTag});
        auto &gpu{*state.gpu};

        vk::RenderPass lRenderPass;
        u32 subpassIndex;

        using namespace node;
        for (NodeVariant &node : slot->nodes) {
            std::visit(VariantVisitor{
                [&](FunctionNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "FunctionNode");
                    node(slot->commandBuffer, slot->cycle, gpu);
                },

                [&](CheckpointNode &node) {
                    RecordFullBarrier(slot->commandBuffer);

                    TRACE_EVENT_INSTANT("gpu", "CheckpointNode", "id", node.id, [&](perfetto::EventContext ctx) {
                        ctx.event()->add_flow_ids(node.id);
                    });

                    std::array<vk::BufferCopy, 1> copy{vk::BufferCopy{
                        .size = node.binding.size,
                        .srcOffset = node.binding.offset,
                        .dstOffset = 0,
                    }};

                    slot->commandBuffer.copyBuffer(node.binding.buffer, gpu.debugTracingBuffer.vkBuffer, copy);

                    RecordFullBarrier(slot->commandBuffer);
                },

                [&](RenderPassNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "RenderPassNode");
                    lRenderPass = node(slot->commandBuffer, slot->cycle, gpu);
                    subpassIndex = 0;
                },

                [&](NextSubpassNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "NextSubpassNode");
                    node(slot->commandBuffer, slot->cycle, gpu);
                    ++subpassIndex;
                },

                [&](SubpassFunctionNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "SubpassFunctionNode");
                    node(slot->commandBuffer, slot->cycle, gpu, lRenderPass, subpassIndex);
                },

                [&](NextSubpassFunctionNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "NextSubpassFunctionNode");
                    node(slot->commandBuffer, slot->cycle, gpu, lRenderPass, ++subpassIndex);
                },

                [&](RenderPassEndNode &node) {
                    TRACE_EVENT_INSTANT("gpu", "RenderPassEndNode");
                    node(slot->commandBuffer, slot->cycle, gpu);
                },
            }, node);
            #undef NODE
        }

        slot->commandBuffer.end();
        slot->ready = false;

        gpu.scheduler.SubmitCommandBuffer(slot->commandBuffer, slot->cycle);

        slot->nodes.clear();
        slot->allocator.Reset();
    }

    void CommandRecordThread::Run() {
        auto &gpu{*state.gpu};

        RENDERDOC_API_1_4_2 *renderDocApi{};
        if (void *mod{dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_NOLOAD)}) {
            auto *pfnGetApi{reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"))};
            if (int ret{pfnGetApi(eRENDERDOC_API_Version_1_4_2, (void **)&renderDocApi)}; ret != 1)
                LOGW("Failed to intialise RenderDoc API: {}", ret);
        }

        outgoing.Push(&slots.emplace_back(gpu));

        if (int result{pthread_setname_np(pthread_self(), "Sky-CmdRecord")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            incoming.Process([this, renderDocApi, &gpu](Slot *slot) {
                idle = false;
                VkInstance instance{*gpu.vkInstance};
                if (renderDocApi && slot->capture)
                    renderDocApi->StartFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), nullptr);

                ProcessSlot(slot);

                if (renderDocApi && slot->capture)
                    renderDocApi->EndFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance), nullptr);
                slot->capture = false;

                if (slot->didWait && (slots.size() + 1) < (1U << *state.settings->executorSlotCountScale)) {
                    outgoing.Push(&slots.emplace_back(gpu));
                    outgoing.Push(&slots.emplace_back(gpu));
                    slot->didWait = false;
                }

                outgoing.Push(slot);
                idle = true;
            }, [] {});
        } catch (const signal::SignalException &e) {
            LOGE("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            if (state.process)
                state.process->Kill(false);
            else
                std::rethrow_exception(std::current_exception());
        }
    }

    bool CommandRecordThread::IsIdle() const {
        return idle;
    }

    CommandRecordThread::Slot *CommandRecordThread::AcquireSlot() {
        auto startTime{util::GetTimeNs()};
        auto slot{outgoing.Pop()};
        if (util::GetTimeNs() - startTime > GrowThresholdNs)
            slot->didWait = true;

        return slot;
    }

    void CommandRecordThread::ReleaseSlot(Slot *slot) {
        incoming.Push(slot);
    }

    void ExecutionWaiterThread::Run() {
        // Enable turbo clocks to begin with if requested
        if (*state.settings->forceMaxGpuClocks)
            adrenotools_set_turbo(true);

        while (true) {
            std::pair<std::shared_ptr<FenceCycle>, std::function<void()>> item{};
            {
                std::unique_lock lock{mutex};
                if (pendingSignalQueue.empty()) {
                    idle = true;

                    // Don't force turbo clocks when the GPU is idle
                    if (*state.settings->forceMaxGpuClocks)
                        adrenotools_set_turbo(false);

                    condition.wait(lock, [this] { return stopping || !pendingSignalQueue.empty(); });

                    if (stopping && pendingSignalQueue.empty())
                        return;

                    // Once we have work to do, force turbo clocks is enabled
                    if (*state.settings->forceMaxGpuClocks)
                        adrenotools_set_turbo(true);

                    idle = false;
                }
                item = std::move(pendingSignalQueue.front());
                pendingSignalQueue.pop();
            }
            {
                TRACE_EVENT("gpu", "GPU");
                if (item.first)
                    item.first->Wait();
            }

            if (item.second)
                item.second();
        }
    }

    ExecutionWaiterThread::ExecutionWaiterThread(const DeviceState &state) : state{state}, thread{&ExecutionWaiterThread::Run, this} {}

    ExecutionWaiterThread::~ExecutionWaiterThread() {
        Stop();
    }

    void ExecutionWaiterThread::Stop() {
        if (!thread.joinable())
            return;

        {
            std::unique_lock lock{mutex};
            stopping = true;
        }
        condition.notify_all();
        thread.join();
    }

    bool ExecutionWaiterThread::IsIdle() const {
        return idle;
    }

    void ExecutionWaiterThread::Queue(std::shared_ptr<FenceCycle> cycle, std::function<void()> &&callback) {
        {
            std::unique_lock lock{mutex};
            pendingSignalQueue.push({std::move(cycle), std::move(callback)});
        }
        condition.notify_all();
    }

    void CheckpointPollerThread::Run() {
        u32 prevCheckpoint{};
        for (size_t iteration{}; true; iteration++) {
            u32 curCheckpoint{state.gpu->debugTracingBuffer.as<u32>()};

            if ((iteration % 1024) == 0)
                LOGI("Current Checkpoint: {}", curCheckpoint);

            while (prevCheckpoint != curCheckpoint) {
                // Make sure to report an event for every checkpoint inbetween the previous and current values, to ensure the perfetto trace is consistent
                prevCheckpoint++;
                TRACE_EVENT_INSTANT("gpu", "Checkpoint", "id", prevCheckpoint, [&](perfetto::EventContext ctx) {
                    ctx.event()->add_terminating_flow_ids(prevCheckpoint);
                });
            }

            prevCheckpoint = curCheckpoint;
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    }

    CheckpointPollerThread::CheckpointPollerThread(const DeviceState &state) : state{state}, thread{&CheckpointPollerThread::Run, this} {}

    CommandExecutor::CommandExecutor(const DeviceState &state)
        : state{state},
          gpu{*state.gpu},
          recordThread{state},
          waiterThread{state},
          checkpointPollerThread{EnableGpuCheckpoints ? std::optional<CheckpointPollerThread>{state} : std::optional<CheckpointPollerThread>{}},
          tag{AllocateTag()} {
        RotateRecordSlot();
    }

    CommandExecutor::~CommandExecutor() {
        // No worker may outlive the executor state it references. Drain recording first so all
        // queued command buffers are submitted, then drain completion waits/callbacks before
        // releasing the current, not-yet-submitted cycle and the remaining executor resources.
        recordThread.Stop();
        waiterThread.Stop();
        cycle->Cancel();
    }

    void CommandExecutor::RotateRecordSlot() {
        if (slot) {
            slot->capture = captureNextExecution;
            recordThread.ReleaseSlot(slot);
        }

        captureNextExecution = false;
        slot = recordThread.AcquireSlot();
        cycle = slot->Reset(gpu);
        slot->executionTag = executionTag;
        allocator = &slot->allocator;
    }

    static bool ViewsEqual(vk::ImageView a, TextureView *b) {
        return (!a && !b) || (a && b && b->GetView() == a);
    }

    bool CommandExecutor::CreateRenderPassWithSubpass(vk::Rect2D renderArea, span<TextureView *> sampledImages, span<TextureView *> inputAttachments, span<TextureView *> colorAttachments, TextureView *depthStencilAttachment, bool noSubpassCreation, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask) {
        auto addSubpass{[&] {
            renderPass->AddSubpass(inputAttachments, colorAttachments, depthStencilAttachment, gpu);
            lastSubpassColorAttachments.clear();
            lastSubpassInputAttachments.clear();

            ranges::transform(colorAttachments, std::back_inserter(lastSubpassColorAttachments), [](TextureView *view){ return view ? view->GetView() : vk::ImageView{};});
            ranges::transform(inputAttachments, std::back_inserter(lastSubpassInputAttachments), [](TextureView *view){ return view ? view->GetView() : vk::ImageView{};});
            lastSubpassDepthStencilAttachment = depthStencilAttachment ? depthStencilAttachment->GetView() : vk::ImageView{};
        }};

        span<TextureView *> depthStencilAttachmentSpan{depthStencilAttachment ? span<TextureView *>(depthStencilAttachment) : span<TextureView *>()};
        auto outputAttachmentViews{ranges::views::concat(colorAttachments, depthStencilAttachmentSpan)};
        bool attachmentsMatch{std::equal(lastSubpassInputAttachments.begin(), lastSubpassInputAttachments.end(), inputAttachments.begin(), inputAttachments.end(), ViewsEqual) &&
                              std::equal(lastSubpassColorAttachments.begin(), lastSubpassColorAttachments.end(), colorAttachments.begin(), colorAttachments.end(), ViewsEqual) &&
                              ViewsEqual(lastSubpassDepthStencilAttachment, depthStencilAttachment)};

        bool splitRenderPass{renderPass == nullptr || renderPass->renderArea != renderArea || !attachmentsMatch ||
            !ranges::all_of(outputAttachmentViews, [this] (auto view) { return !view || view->texture->ValidateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::RenderTarget); }) ||
            !ranges::all_of(sampledImages, [this] (auto view) { return view->texture->ValidateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::Sampled); })};

        bool gotoNext{};
        if (splitRenderPass) {
            // We need to create a render pass if one doesn't already exist or the current one isn't compatible
            if (renderPass != nullptr) {
                slot->nodes.emplace_back(std::in_place_type_t<node::RenderPassEndNode>());
                slot->nodes.splice(slot->nodes.end(), slot->pendingPostRenderPassNodes);
                renderPassIndex++;
            }
            renderPass = &std::get<node::RenderPassNode>(slot->nodes.emplace_back(std::in_place_type_t<node::RenderPassNode>(), renderArea));
            renderPassIt = std::prev(slot->nodes.end());
            addSubpass();
            subpassCount = 1;
        } else if (!attachmentsMatch) {
            // The last subpass had different attachments, so we need to create a new one
            addSubpass();
            subpassCount++;
            gotoNext = true;
        }

        renderPass->UpdateDependency(srcStageMask, dstStageMask);

        for (auto view : outputAttachmentViews)
            if (view) {
                view->texture->UpdateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::RenderTarget);
                MarkCopyOnlyWritten(view);
            }

        for (auto view : sampledImages)
            view->texture->UpdateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::Sampled);

        return gotoNext;
    }

    void CommandExecutor::FinishRenderPass() {
        if (renderPass) {
            slot->nodes.emplace_back(std::in_place_type_t<node::RenderPassEndNode>());
            slot->nodes.splice(slot->nodes.end(), slot->pendingPostRenderPassNodes);
            renderPassIndex++;

            renderPass = nullptr;
            subpassCount = 0;

            lastSubpassInputAttachments.clear();
            lastSubpassColorAttachments.clear();
            lastSubpassDepthStencilAttachment = vk::ImageView{};
        }
    }

    CommandExecutor::LockedTexture::LockedTexture(std::shared_ptr<Texture> texture) : texture{std::move(texture)} {}

    constexpr CommandExecutor::LockedTexture::LockedTexture(CommandExecutor::LockedTexture &&other) : texture{std::exchange(other.texture, nullptr)} {}

    constexpr Texture *CommandExecutor::LockedTexture::operator->() const {
        return texture.get();
    }

    CommandExecutor::LockedTexture::~LockedTexture() {
        if (texture)
            texture->unlock();
    }

    namespace {
        template<typename Function>
        void ForEachViewSubresource(TextureView *view, Function &&function) {
            const auto baseMip{view->range.baseMipLevel};
            const auto levelCount{view->range.levelCount == VK_REMAINING_MIP_LEVELS
                ? view->texture->levelCount - baseMip : view->range.levelCount};
            const bool threeDimensional{view->texture->guest &&
                view->texture->guest->GetImageType() == vk::ImageType::e3D};
            if (threeDimensional) {
                for (u32 mip{}; mip < levelCount; ++mip) {
                    const auto resolvedMip{baseMip + mip};
                    const auto mipDepth{std::max(view->texture->dimensions.depth >> resolvedMip, 1U)};
                    if (view->type == vk::ImageViewType::e3D) {
                        for (u32 slice{}; slice < mipDepth; ++slice)
                            function({.mip = resolvedMip, .layer = 0, .depthSlice = slice});
                    } else {
                        const auto baseSlice{view->range.baseArrayLayer};
                        const auto sliceCount{view->range.layerCount == VK_REMAINING_ARRAY_LAYERS
                            ? mipDepth - std::min(baseSlice, mipDepth)
                            : view->range.layerCount};
                        for (u32 slice{}; slice < sliceCount && baseSlice + slice < mipDepth; ++slice)
                            function({.mip = resolvedMip, .layer = 0, .depthSlice = baseSlice + slice});
                    }
                }
                return;
            }
            const auto baseLayer{view->range.baseArrayLayer};
            const auto layerCount{view->range.layerCount == VK_REMAINING_ARRAY_LAYERS
                ? view->texture->layerCount - baseLayer : view->range.layerCount};
            for (u32 mip{}; mip < levelCount; ++mip)
                for (u32 layer{}; layer < layerCount; ++layer)
                    function(texture::ResolvedSubresource{
                        .mip = baseMip + mip,
                        .layer = baseLayer + layer,
                        .depthSlice = 0,
                    });
        }
    }

    void CommandExecutor::AcquireCopyOnlyRuntime(TextureView *view) {
        auto storage{view->texture->storage.lock()};
        while (storage) {
            auto group{storage->GetGroup()};
            if (!group || !group->HasExecutableCopyRoutes() || copyOnlyRuntimeGroup == group)
                return;

            if (copyOnlyRuntimeLock.owns_lock())
                Submit({}, false);

            std::unique_lock candidateLock{group->RuntimeSynchronizationMutex()};
            if (storage->GetGroup() != group)
                continue;

            copyOnlyRuntimeGroup = std::move(group);
            copyOnlyRuntimeLock = std::move(candidateLock);
            return;
        }
    }

    void CommandExecutor::SynchronizeCopyOnly(TextureView *view) {
        auto destinationStorage{view->texture->storage.lock()};
        auto destinationTexture{std::shared_ptr<Texture>{view->texture}};
        auto destinationGroup{destinationStorage ? destinationStorage->GetGroup() : nullptr};
        if (!destinationGroup ||
            view->texture->layout != vk::ImageLayout::eGeneral)
            return;

        ForEachViewSubresource(view, [&](texture::ResolvedSubresource destinationSubresource) {
            auto scheduled{destinationGroup->ScheduleCopySynchronization(
                destinationStorage, destinationSubresource)};
            if (scheduled.state != texture::CopySynchronizationState::Ready || !scheduled.pending)
                return;

            const auto &prepared{scheduled.pending->Prepared()};
            auto sourceStorage{prepared.read.source};
            auto sourceTexture{sourceStorage ? sourceStorage->texture : nullptr};
            if (!sourceTexture)
                return;

            if (sourceTexture->LockWithTag(tag))
                attachedTextures.emplace_back(sourceTexture);
            if (sourceTexture->layout != vk::ImageLayout::eGeneral)
                return;

            const auto sourceImage{sourceTexture->GetBacking()};
            const auto destinationImage{destinationTexture->GetBacking()};
            if (!sourceImage || !destinationImage)
                return;

            auto pending{std::move(scheduled.pending)};
            const auto region{prepared.copyRegion};
            const auto depthSliceCopy{IsDepthSliceCopy(region)};
            const auto copyId{depthSliceCopy ? ++depthSliceCopyId : 0};
            const auto sourceInfo{depthSliceCopy
                ? destinationGroup->GetCopyRepresentationDebugInfo(
                    sourceStorage, region.sourceSubresource)
                : std::nullopt};
            const auto destinationInfo{depthSliceCopy
                ? destinationGroup->GetCopyRepresentationDebugInfo(
                    destinationStorage, region.destinationSubresource)
                : std::nullopt};
            if (depthSliceCopy) {
                if (const auto reason = ValidateDepthSliceCopy(
                        sourceStorage, *sourceTexture, sourceImage,
                        destinationStorage, *destinationTexture, destinationImage, prepared)) {
                    LOGE("TexmanDepthCopy reject id={} {}->{} reason='{}' srcStorage={} "
                         "srcImage=0x{:X} srcSub={}/{}/{} srcZ={} dstStorage={} "
                         "dstImage=0x{:X} dstSub={}/{}/{} dstZ={} extent={}x{}x{} "
                         "srcGen={} dstGen={}",
                         copyId, ImageKindName(region.sourceImageType),
                         ImageKindName(region.destinationImageType), reason,
                         fmt::ptr(sourceStorage.get()), ImageHandleValue(sourceImage),
                         region.sourceSubresource.mip, region.sourceSubresource.layer,
                         region.sourceSubresource.depthSlice, region.sourceOffsetZ,
                         fmt::ptr(destinationStorage.get()), ImageHandleValue(destinationImage),
                         region.destinationSubresource.mip,
                         region.destinationSubresource.layer,
                         region.destinationSubresource.depthSlice, region.destinationOffsetZ,
                         region.width, region.height, region.depth,
                         prepared.read.sourceGeneration,
                         prepared.read.destinationGeneration);
                    return;
                }
            }
            AddOutsideRpCommand([
                sourceStorage, destinationStorage, sourceTexture, destinationTexture,
                sourceImage, destinationImage, region, sourceInfo, destinationInfo,
                depthSliceCopy, copyId, pending
            ](vk::raii::CommandBuffer &commandBuffer,
              const std::shared_ptr<FenceCycle> &recordingCycle, GPU &) {
                const vk::ImageSubresourceRange sourceRange{
                    .aspectMask = vk::ImageAspectFlags{region.aspectMask},
                    .baseMipLevel = region.sourceSubresource.mip,
                    .levelCount = 1,
                    .baseArrayLayer = region.sourceImageType == texture::ImageKind::ThreeDimensional
                        ? 0 : region.sourceSubresource.layer,
                    .layerCount = 1,
                };
                const vk::ImageSubresourceRange destinationRange{
                    .aspectMask = vk::ImageAspectFlags{region.aspectMask},
                    .baseMipLevel = region.destinationSubresource.mip,
                    .levelCount = 1,
                    .baseArrayLayer = region.destinationImageType == texture::ImageKind::ThreeDimensional
                        ? 0 : region.destinationSubresource.layer,
                    .layerCount = 1,
                };
                const std::array before{
                    vk::ImageMemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead |
                            vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = sourceImage,
                        .subresourceRange = sourceRange,
                    },
                    vk::ImageMemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead |
                            vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = destinationImage,
                        .subresourceRange = destinationRange,
                    },
                };
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eAllCommands,
                    vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);

                const std::array copyRegion{vk::ImageCopy{
                    .srcSubresource = {
                        .aspectMask = vk::ImageAspectFlags{region.aspectMask},
                        .mipLevel = region.sourceSubresource.mip,
                        .baseArrayLayer = region.sourceImageType == texture::ImageKind::ThreeDimensional
                            ? 0 : region.sourceSubresource.layer,
                        .layerCount = 1,
                    },
                    .srcOffset = {0, 0, static_cast<std::int32_t>(region.sourceOffsetZ)},
                    .dstSubresource = {
                        .aspectMask = vk::ImageAspectFlags{region.aspectMask},
                        .mipLevel = region.destinationSubresource.mip,
                        .baseArrayLayer = region.destinationImageType == texture::ImageKind::ThreeDimensional
                            ? 0 : region.destinationSubresource.layer,
                        .layerCount = 1,
                    },
                    .dstOffset = {0, 0, static_cast<std::int32_t>(region.destinationOffsetZ)},
                    .extent = {region.width, region.height, region.depth},
                }};
                if (depthSliceCopy) {
                    const auto sourceMip{
                        ActualMipDimensions(*sourceTexture, region.sourceSubresource.mip)};
                    const auto destinationMip{
                        ActualMipDimensions(*destinationTexture,
                                            region.destinationSubresource.mip)};
                    const auto &read{pending->Prepared().read};
                    LOGI("TexmanDepthCopy emit id={} {}->{} srcStorage={} srcImage=0x{:X} "
                         "srcType={}/{} srcSub={}/{}/{} srcMip={}x{}x{} srcZ={} "
                         "srcLayout={} srcState={} srcGen={}/{} preparedGen={} "
                         "dstStorage={} dstImage=0x{:X} dstType={}/{} dstSub={}/{}/{} "
                         "dstMip={}x{}x{} dstZ={} dstLayout={} dstState={} dstGen={}/{} "
                         "preparedGen={} extent={}x{}x{}",
                         copyId, ImageKindName(region.sourceImageType),
                         ImageKindName(region.destinationImageType),
                         fmt::ptr(sourceStorage.get()), ImageHandleValue(sourceImage),
                         ImageKindName(region.sourceImageType),
                         TextureImageTypeName(*sourceTexture),
                         region.sourceSubresource.mip, region.sourceSubresource.layer,
                         region.sourceSubresource.depthSlice,
                         sourceMip.width, sourceMip.height, sourceMip.depth,
                         region.sourceOffsetZ, vk::to_string(sourceTexture->layout),
                         CopyStateName(sourceInfo),
                         sourceInfo ? sourceInfo->generation : 0,
                         sourceInfo ? sourceInfo->currentGeneration : 0,
                         read.sourceGeneration,
                         fmt::ptr(destinationStorage.get()),
                         ImageHandleValue(destinationImage),
                         ImageKindName(region.destinationImageType),
                         TextureImageTypeName(*destinationTexture),
                         region.destinationSubresource.mip,
                         region.destinationSubresource.layer,
                         region.destinationSubresource.depthSlice,
                         destinationMip.width, destinationMip.height, destinationMip.depth,
                         region.destinationOffsetZ,
                         vk::to_string(destinationTexture->layout),
                         CopyStateName(destinationInfo),
                         destinationInfo ? destinationInfo->generation : 0,
                         destinationInfo ? destinationInfo->currentGeneration : 0,
                         read.destinationGeneration,
                         region.width, region.height, region.depth);
                }
                commandBuffer.copyImage(
                    sourceImage, vk::ImageLayout::eGeneral,
                    destinationImage, vk::ImageLayout::eGeneral, copyRegion);

                const std::array after{
                    vk::ImageMemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                        .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
                            vk::AccessFlagBits::eMemoryWrite,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = sourceImage,
                        .subresourceRange = sourceRange,
                    },
                    vk::ImageMemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
                            vk::AccessFlagBits::eMemoryWrite,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = destinationImage,
                        .subresourceRange = destinationRange,
                    },
                };
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
                recordingCycle->AttachObject(pending);
            });
            pendingGpuCompletionCallbacks.emplace_back(
                [pending = std::move(pending)] { pending->Complete(); });
        });
    }

    void CommandExecutor::MarkCopyOnlyWritten(TextureView *view) {
        auto storage{view->texture->storage.lock()};
        auto group{storage ? storage->GetGroup() : nullptr};
        if (!group)
            return;
        ForEachViewSubresource(view, [&](texture::ResolvedSubresource subresource) {
            const std::array exact{subresource};
            const auto depthSliceRoute{group->HasDepthSliceCopyRoute(storage, subresource)};
            const auto before{depthSliceRoute
                ? group->GetCopyRepresentationDebugInfo(storage, subresource)
                : std::nullopt};
            const auto marked{group->MarkCopyRepresentationWritten(storage, exact)};
            if (!depthSliceRoute)
                return;
            const auto after{group->GetCopyRepresentationDebugInfo(storage, subresource)};
            const auto mip{ActualMipDimensions(*view->texture, subresource.mip)};
            LOGI("TexmanDepthCopy write storage={} image=0x{:X} type={} "
                 "sub={}/{}/{} mip={}x{}x{} layout={} marked={} state={}->{} "
                 "gen={}/{}->{}/{}",
                 fmt::ptr(storage.get()), ImageHandleValue(view->texture->GetBacking()),
                 TextureImageTypeName(*view->texture),
                 subresource.mip, subresource.layer, subresource.depthSlice,
                 mip.width, mip.height, mip.depth, vk::to_string(view->texture->layout), marked,
                 CopyStateName(before), CopyStateName(after),
                 before ? before->generation : 0,
                 before ? before->currentGeneration : 0,
                 after ? after->generation : 0,
                 after ? after->currentGeneration : 0);
        });
    }

    bool CommandExecutor::AttachTexture(TextureView *view) {
        AcquireCopyOnlyRuntime(view);

        bool didLock{view->LockWithTag(tag)};
        if (didLock) {
            // TODO: fixup remaining bugs with this and add better heuristics to avoid pauses
            // if (view->texture->FrequentlyLocked())
            attachedTextures.emplace_back(view->texture);
            // else
            //    preserveAttachedTextures.emplace_back(view->texture);
        }

        SynchronizeCopyOnly(view);

        return didLock;
    }

    void CommandExecutor::MarkTextureWritten(TextureView *view) {
        MarkCopyOnlyWritten(view);
    }

    CommandExecutor::LockedBuffer::LockedBuffer(std::shared_ptr<Buffer> buffer) : buffer{std::move(buffer)} {}

    constexpr CommandExecutor::LockedBuffer::LockedBuffer(CommandExecutor::LockedBuffer &&other) : buffer{std::exchange(other.buffer, nullptr)} {}

    constexpr Buffer *CommandExecutor::LockedBuffer::operator->() const {
        return buffer.get();
    }

    CommandExecutor::LockedBuffer::~LockedBuffer() {
        if (buffer)
            buffer->unlock();
    }

    void CommandExecutor::AttachBufferBase(std::shared_ptr<Buffer> buffer) {
        // TODO: fixup remaining bugs with this and add better heuristics to avoid pauses
        // if (buffer->FrequentlyLocked())
        attachedBuffers.emplace_back(std::move(buffer));
        // else
        //    preserveAttachedBuffers.emplace_back(std::move(buffer));
    }

    bool CommandExecutor::AttachBuffer(BufferView &view) {
        bool didLock{view.LockWithTag(tag)};
        if (didLock)
            AttachBufferBase(view.GetBuffer()->shared_from_this());

        return didLock;
    }

    void CommandExecutor::AttachLockedBufferView(BufferView &view, ContextLock<BufferView> &&lock) {
        if (lock.OwnsLock()) {
            // Transfer ownership to executor so that the resource will stay locked for the period it is used on the GPU
            AttachBufferBase(view.GetBuffer()->shared_from_this());
            lock.Release(); // The executor will handle unlocking the lock so it doesn't need to be handled here
        }
    }

    void CommandExecutor::AttachLockedBuffer(std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
        if (lock.OwnsLock()) {
            AttachBufferBase(std::move(buffer));
            lock.Release(); // See AttachLockedBufferView(...)
        }
    }

    void CommandExecutor::AttachDependency(const std::shared_ptr<void> &dependency) {
        cycle->AttachObject(dependency);
    }

    void CommandExecutor::AddSubpass(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)> &&function, vk::Rect2D renderArea, span<TextureView *> sampledImages, span<TextureView *> inputAttachments, span<TextureView *> colorAttachments, TextureView *depthStencilAttachment, bool noSubpassCreation, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask) {
        bool gotoNext{CreateRenderPassWithSubpass(renderArea, sampledImages, inputAttachments, colorAttachments, depthStencilAttachment ? &*depthStencilAttachment : nullptr, noSubpassCreation, srcStageMask, dstStageMask)};
        if (gotoNext)
            slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassFunctionNode>(), std::forward<decltype(function)>(function));
        else
            slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), std::forward<decltype(function)>(function));

        if (slot->nodes.size() > *state.settings->executorFlushThreshold && !gotoNext)
            Submit();
    }

    void CommandExecutor::AddOutsideRpCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        if (renderPass)
            FinishRenderPass();

        slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::AddCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::InsertPreExecuteCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        slot->nodes.emplace(slot->nodes.begin(), std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::InsertPreRpCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        slot->nodes.emplace(renderPass ? renderPassIt : slot->nodes.end(), std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::InsertPostRpCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        slot->pendingPostRenderPassNodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::AddFullBarrier() {
        AddOutsideRpCommand([](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
            RecordFullBarrier(commandBuffer);
        });
    }

    void CommandExecutor::AddClearColorSubpass(TextureView *attachment, const vk::ClearColorValue &value) {
        bool gotoNext{CreateRenderPassWithSubpass(vk::Rect2D{.extent = attachment->texture->dimensions}, {}, {}, attachment, nullptr)};
        if (renderPass->ClearColorAttachment(0, value, gpu)) {
            if (gotoNext)
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassNode>());
        } else {
            auto function{[scissor = attachment->texture->dimensions, value](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32) {
                commandBuffer.clearAttachments(vk::ClearAttachment{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .colorAttachment = 0,
                    .clearValue = value,
                }, vk::ClearRect{
                    .rect = vk::Rect2D{.extent = scissor},
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                });
            }};

            if (gotoNext)
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassFunctionNode>(), function);
            else
                slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), function);
        }
    }

    void CommandExecutor::AddClearDepthStencilSubpass(TextureView *attachment, const vk::ClearDepthStencilValue &value) {
        bool gotoNext{CreateRenderPassWithSubpass(vk::Rect2D{.extent = attachment->texture->dimensions}, {}, {}, {}, attachment)};
        if (renderPass->ClearDepthStencilAttachment(value, gpu)) {
            if (gotoNext)
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassNode>());
        } else {
            auto function{[aspect = attachment->format->vkAspect, extent = attachment->texture->dimensions, value](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32) {
                commandBuffer.clearAttachments(vk::ClearAttachment{
                    .aspectMask = aspect,
                    .clearValue = value,
                }, vk::ClearRect{
                    .rect.extent = extent,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                });
            }};

            if (gotoNext)
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassFunctionNode>(), function);
            else
                slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), function);
        }
    }

    void CommandExecutor::AddFlushCallback(std::function<void()> &&callback) {
        flushCallbacks.emplace_back(std::forward<decltype(callback)>(callback));
    }

    void CommandExecutor::AddPipelineChangeCallback(std::function<void()> &&callback) {
        pipelineChangeCallbacks.emplace_back(std::forward<decltype(callback)>(callback));
    }

    void CommandExecutor::NotifyPipelineChange() {
        for (auto &callback : pipelineChangeCallbacks)
            callback();
    }

    std::optional<u32> CommandExecutor::GetRenderPassIndex() {
        return renderPassIndex;
    }

    u32 CommandExecutor::AddCheckpointImpl(std::string_view annotation) {
        if (renderPass)
            FinishRenderPass();

        slot->nodes.emplace_back(node::CheckpointNode{gpu.megaBufferAllocator.Push(cycle, span<u32>(&nextCheckpointId, 1).cast<u8>()), nextCheckpointId});

        TRACE_EVENT_INSTANT("gpu", "Mark Checkpoint", "id", nextCheckpointId, "annotation", [&annotation](perfetto::TracedValue context) {
            std::move(context).WriteString(annotation.data(), annotation.size());
        }, [&](perfetto::EventContext ctx) {
            ctx.event()->add_flow_ids(nextCheckpointId);
        });

        return nextCheckpointId++;
    }

    void CommandExecutor::SubmitInternal() {
        if (renderPass)
            FinishRenderPass();

        slot->nodes.splice(slot->nodes.end(), slot->pendingPostRenderPassNodes);


        {
            slot->WaitReady();

            // We need this barrier here to ensure that resources are in the state we expect them to be in, we shouldn't overwrite resources while prior commands might still be using them or read from them while they might be modified by prior commands
            RecordFullBarrier(slot->commandBuffer);

            boost::container::small_vector<FenceCycle *, 8> chainedCycles;
            for (const auto &texture : ranges::views::concat(attachedTextures, preserveAttachedTextures)) {
                texture->SynchronizeHostInline(slot->commandBuffer, cycle, true);
                // We don't need to attach the Texture to the cycle as a TextureView will already be attached
                if (ranges::find(chainedCycles, texture->cycle.get()) == chainedCycles.end()) {
                    cycle->ChainCycle(texture->cycle);
                    chainedCycles.emplace_back(texture->cycle.get());
                }

                texture->cycle = cycle;
                texture->UpdateRenderPassUsage(0, texture::RenderPassUsage::None);
            }

            // Wait on texture syncs to finish before beginning the cmdbuf
            RecordFullBarrier(slot->commandBuffer);
        }

        for (const auto &attachedBuffer : ranges::views::concat(attachedBuffers, preserveAttachedBuffers)) {
            if (attachedBuffer->RequiresCycleAttach()) {
                attachedBuffer->SynchronizeHost(); // Synchronize attached buffers from the CPU without using a staging buffer
                cycle->AttachObject(attachedBuffer.buffer);
                attachedBuffer->UpdateCycle(cycle);
                attachedBuffer->AllowAllBackingWrites();
            }
        }

        RotateRecordSlot();
    }

    void CommandExecutor::ResetInternal() {
        attachedTextures.clear();
        attachedBuffers.clear();
        if (copyOnlyRuntimeLock.owns_lock())
            copyOnlyRuntimeLock.unlock();
        copyOnlyRuntimeGroup.reset();
        allocator->Reset();
        renderPassIndex = 0;
        usageTracker.sequencedIntervals.Clear();

        // Periodically clear preserve attachments just in case there are new waiters which would otherwise end up waiting forever
        if ((submissionNumber % (2U << *state.settings->executorSlotCountScale)) == 0) {
            preserveAttachedBuffers.clear();
            preserveAttachedTextures.clear();
        }
    }

    void CommandExecutor::Submit(std::function<void()> &&callback, bool wait) {
        for (const auto &flushCallback : flushCallbacks)
            flushCallback();

        executionTag = AllocateTag();

        // Ensure all pushed callbacks wait for the submission to have finished GPU execution
        if (!slot->nodes.empty())
            waiterThread.Queue(cycle, {});

        if (*state.settings->useDirectMemoryImport) {
            // When DMI is in use, callbacks and deferred actions should be executed in sequence with the host GPU
            for (auto &actionCb : pendingDeferredActions)
                waiterThread.Queue(nullptr, std::move(actionCb));

            pendingDeferredActions.clear();

            if (callback)
                waiterThread.Queue(nullptr, std::move(callback));
        }

        if (!slot->nodes.empty()) {
            TRACE_EVENT("gpu", "CommandExecutor::Submit");
            auto submittedCycle{cycle};
            SubmitInternal();
            for (auto &completion : pendingGpuCompletionCallbacks)
                waiterThread.Queue(submittedCycle, std::move(completion));
            pendingGpuCompletionCallbacks.clear();
            submissionNumber++;
        } else {
            // No Vulkan submission owns these reservations, so their tickets cancel them.
            pendingGpuCompletionCallbacks.clear();
        }

        if (!*state.settings->useDirectMemoryImport) {
            // When DMI is not in use, execute callbacks immediately after submission
            for (auto &actionCb : pendingDeferredActions)
                actionCb();

            pendingDeferredActions.clear();

            if (callback)
                callback();
        }

        ResetInternal();

        if (wait) {
            usageTracker.dirtyIntervals.Clear();

            std::condition_variable cv;
            std::mutex mutex;
            bool gpuDone{};

            waiterThread.Queue(nullptr, [&cv, &mutex, &gpuDone] {
                std::scoped_lock lock{mutex};
                gpuDone = true;
                cv.notify_one();
            });

            std::unique_lock lock{mutex};
            cv.wait(lock, [&gpuDone] { return gpuDone; });
        }
    }

    void CommandExecutor::AddDeferredAction(std::function<void()> &&callback) {
        pendingDeferredActions.emplace_back(std::move(callback));
    }

    void CommandExecutor::LockPreserve() {
        if (!preserveLocked) {
            preserveLocked = true;

            for (auto &buffer : preserveAttachedBuffers)
                buffer->LockWithTag(tag);

            for (auto &texture : preserveAttachedTextures)
                texture->LockWithTag(tag);
        }
    }

    void CommandExecutor::UnlockPreserve() {
        if (preserveLocked) {
            for (auto &buffer : preserveAttachedBuffers)
                buffer->unlock();

            for (auto &texture : preserveAttachedTextures)
                texture->unlock();

            preserveLocked = false;
        }
    }
}
