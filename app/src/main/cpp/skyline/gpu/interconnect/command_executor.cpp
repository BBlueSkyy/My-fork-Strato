// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <range/v3/view.hpp>
#include <adrenotools/driver.h>
#include <common/settings.h>
#include <loader/loader.h>
#include <gpu.h>
#include <dlfcn.h>
#include "command_executor.h"
#include <nce.h>

namespace skyline::gpu::interconnect {
    static texture::diagnostic::SpongeBobWriter SbDescribeWriter(
        TextureView *view, const texture::diagnostic::SpongeBobMiniDrawInfo *draw,
        u32 passIndex, u32 kind) {
        texture::diagnostic::SpongeBobWriter w{};
        if (!view || !view->texture) return w;
        const auto &image{*view->texture};
        w.textureKey = reinterpret_cast<uintptr_t>(view->texture.get());
        w.generation = image.backingGeneration;
        if (image.guest && !image.guest->mappings.empty()) {
            const auto &first{image.guest->mappings.front()};
            w.guestFirstBegin = reinterpret_cast<uintptr_t>(first.data());
            w.guestFirstSize = first.size();
        }
        w.renderPassIndex = passIndex;
        w.kind = kind;
        w.format = static_cast<u32>(image.format->vkFormat);
        w.width = image.dimensions.width;
        w.height = image.dimensions.height;
        if (draw) {
            w.pipeline = draw->pipelineHash;
            w.vertexShader = draw->shaderHashes[1];
            w.fragmentShader = draw->shaderHashes[5];
        }
        return w;
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

    bool CommandExecutor::CreateRenderPassWithSubpass(vk::Rect2D renderArea, span<TextureView *> sampledImages, span<TextureView *> inputAttachments, span<TextureView *> colorAttachments, TextureView *depthStencilAttachment, bool noSubpassCreation, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask, span<TextureView *> storageWrites) {
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
        // One operation cannot define an order for two distinct writers of
        // the same guest bytes. Multiple views of one backing remain valid.
        boost::container::small_vector<TextureView *, 8> writers;
        for (auto view : outputAttachmentViews) if (view) writers.push_back(view);
        for (auto view : storageWrites) if (view) writers.push_back(view);
        PreflightTextureUses(sampledImages, writers);
        PreflightTextureUses(inputAttachments, {});
        bool attachmentsMatch{std::equal(lastSubpassInputAttachments.begin(), lastSubpassInputAttachments.end(), inputAttachments.begin(), inputAttachments.end(), ViewsEqual) &&
                              std::equal(lastSubpassColorAttachments.begin(), lastSubpassColorAttachments.end(), colorAttachments.begin(), colorAttachments.end(), ViewsEqual) &&
                              ViewsEqual(lastSubpassDepthStencilAttachment, depthStencilAttachment)};

        bool splitRenderPass{renderPass == nullptr || renderPass->renderArea != renderArea || !attachmentsMatch ||
            !ranges::all_of(outputAttachmentViews, [this] (auto view) { return !view || view->texture->ValidateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::RenderTarget); }) ||
            !ranges::all_of(sampledImages, [this] (auto view) { return view->texture->ValidateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::Sampled); })};

        if ((splitRenderPass && renderPass != nullptr) || (!splitRenderPass && !attachmentsMatch))
            for (auto &callback : subpassBoundaryCallbacks) callback();
        auto publication{AcquireRegionPublication(writers)};

        bool gotoNext{};
        if (splitRenderPass) {
            // We need to create a render pass if one doesn't already exist or the current one isn't compatible
            if (renderPass != nullptr) {
                EndSbNaturalPass("natural_split");
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
            if (view)
                view->texture->UpdateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::RenderTarget);

        for (auto view : sampledImages)
            view->texture->UpdateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::Sampled);

        for (auto view : storageWrites) if (view) view->texture->MarkGpuDirty(usageTracker);
        return gotoNext;
    }

    void CommandExecutor::EndSbNaturalPass(const char *why) {
        if (!sbNaturalPassOperation) return;
        LOGI("[SB_PASS_END] operation={} draws={} reason={} renderPassIndex={}",
            sbNaturalPassOperation, sbNaturalPassDraws, why, renderPassIndex);
        sbNaturalPassOperation = 0;
        sbNaturalPassDraws = 0;
        gpu.spongeBobMini.EndNaturalPass();
    }

    void CommandExecutor::FinishRenderPass() {
        if (renderPass) {
            EndSbNaturalPass("natural_end");
            for (auto &callback : subpassBoundaryCallbacks)
                callback();

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

    void CommandExecutor::TrackRegionUse(Texture &texture) {
        if (!regionOperationActive || !texture.regionTracked.load(std::memory_order_relaxed)) return;
        if (std::find(regionOperationUses.begin(), regionOperationUses.end(), &texture) == regionOperationUses.end())
            regionOperationUses.push_back(&texture);
    }

    CommandExecutor::RegionPublication CommandExecutor::AcquireRegionPublication(span<TextureView *> required) {
        // All full resource locks and views are acquired by preflight, before
        // taking state/metadata locks. No resource acquisition is allowed below.
        auto resources{regionOperationUses};
        for (auto view : required) if (view && view->texture->regionTracked.load(std::memory_order_relaxed)) {
            auto resource{view->texture.get()};
            if (std::find(resources.begin(), resources.end(), resource) == resources.end()) resources.push_back(resource);
        }
        std::sort(resources.begin(), resources.end(), std::less<Texture *>{});
        boost::container::small_vector<std::shared_ptr<texture::RegionAuthority>, 4> groups;
        RegionPublication publication;
        for (auto resource : resources) {
            publication.states.emplace_back(resource->stateMutex);
            auto group{resource->regionAuthority.load()};
            if (std::find(groups.begin(), groups.end(), group) == groups.end()) groups.push_back(std::move(group));
        }
        std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) { return std::less<texture::RegionAuthority *>{}(a.get(), b.get()); });
        for (const auto &group : groups) publication.metadata.emplace_back(group->mutex);
        for (auto resource : resources) {
            auto group{resource->regionAuthority.load()};
            if (!group || resource->dirtyState == Texture::DirtyState::CpuDirty ||
                std::any_of(resource->regionBindings.begin(), resource->regionBindings.end(), [&](const auto &binding) {
                    return binding.residentVersion != group->regions[binding.region].hostVersion;
                }))
                throw TextureAliasReconciliationRequest{resource->shared_from_this(), {}};
            // CPU callbacks can conservatively invalidate a whole neighbor even
            // when none of its resident intervals changed. Versions decide.
            resource->regionStale.store(false, std::memory_order_relaxed);
        }
        return publication;
    }

    bool CommandExecutor::AttachTexture(TextureView *view) {
        // Cached TICs/descriptors bypass FindOrCreate. Check them before any node
        // for this operation is published; retry submits prior work only.
        view->texture->RequireAliasCoherence(tag);

        bool didLock{view->LockWithTag(tag)};
        if (didLock) {
            // TODO: fixup remaining bugs with this and add better heuristics to avoid pauses
            // if (view->texture->FrequentlyLocked())
            attachedTextures.emplace_back(view->texture);
            // else
            //    preserveAttachedTextures.emplace_back(view->texture);
        }

        TrackRegionUse(*view->texture);
        if (view->texture->HasAliases()) {
            view->texture->SynchronizeHost(false); // Initialize before claiming a GPU write, once per generation.
            view->texture->RequireAliasCoherence(tag);
            if (view->texture->regionTracked.load(std::memory_order_relaxed))
                view->texture->pendingRegionUseTag = tag;
        }
        return didLock;
    }

    void CommandExecutor::PreflightTextureUses(span<TextureView *> reads, span<TextureView *> writers) {
        Texture::ValidateAliasWrites(writers);
        for (auto view : reads)
            if (view && view->texture->regionTracked.load(std::memory_order_relaxed)) AttachTexture(view);
        for (auto view : writers) if (view) {
            if (view->texture->regionTracked.load(std::memory_order_relaxed)) AttachTexture(view);
            view->GetView();
        }
    }

    void CommandExecutor::CommitTextureWrites(span<TextureView *> writers) {
        Texture::ValidateAliasWrites(writers);
        auto publication{AcquireRegionPublication(writers)};
        for (auto view : writers) if (view) view->texture->RequireAliasCoherence(tag);
        for (auto view : writers) if (view) view->texture->MarkGpuDirty(usageTracker);
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


    bool CommandExecutor::SbMiniEligible(TextureView *view, bool output) const {
        if (!view || !view->texture) return false;
        const auto &texture{*view->texture};
        if (!texture.guest || texture.replaced || texture.aliasRetired.load() ||
            texture.layout != vk::ImageLayout::eGeneral ||
            texture.backingImageType != vk::ImageType::e2D ||
            texture.sampleCount != vk::SampleCountFlagBits::e1 ||
            texture.format->vkAspect != vk::ImageAspectFlagBits::eColor ||
            !(texture.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
            !texture.format->blockWidth || !texture.format->blockHeight || !texture.format->bpb ||
            view->range.baseMipLevel >= texture.levelCount ||
            view->range.baseArrayLayer >= texture.layerCount ||
            view->range.baseMipLevel >= 32)
            return false;
        const u32 width{std::max(1U, texture.dimensions.width >> view->range.baseMipLevel)};
        const u32 height{std::max(1U, texture.dimensions.height >> view->range.baseMipLevel)};
        if (output && (width < 320 || height < 240)) return false;
        if (!width || !height) return false;
        const u32 roiWidth{std::min(width, 64U)};
        const u32 roiHeight{std::min(height, 64U)};
        const size_t bytes{util::DivideCeil<size_t>(roiWidth, texture.format->blockWidth) *
            util::DivideCeil<size_t>(roiHeight, texture.format->blockHeight) * texture.format->bpb};
        return bytes && bytes <= 64 * 1024;
    }

    void CommandExecutor::QueueSbMiniImage(TextureView *view, u64 operation, u32 role, const texture::diagnostic::SpongeBobMiniDrawInfo &draw, u32 sampledOrdinal, bool postNaturalPass) {
        if (!SbMiniEligible(view, role < 3)) return;
        auto texture{view->texture->shared_from_this()};
        const u32 mip{view->range.baseMipLevel}, layer{view->range.baseArrayLayer};
        const u32 width{std::max(1U, texture->dimensions.width >> mip)};
        const u32 height{std::max(1U, texture->dimensions.height >> mip)};
        const u32 roiWidth{std::min(width, 64U)}, roiHeight{std::min(height, 64U)};
        const u32 blockWidth{texture->format->blockWidth}, blockHeight{texture->format->blockHeight};
        const u32 x{role < 3 ? (width - roiWidth) / 2 / blockWidth * blockWidth : 0U};
        const u32 y{role < 3 ? (height - roiHeight) / 2 / blockHeight * blockHeight : 0U};
        const size_t bytes{util::DivideCeil<size_t>(roiWidth, blockWidth) *
            util::DivideCeil<size_t>(roiHeight, blockHeight) * texture->format->bpb};
        auto staging{gpu.memory.AllocateStagingBuffer(bytes)};
        const auto image{texture->GetBacking()};
        const auto generation{texture->backingGeneration};
        const auto hostFormat{static_cast<u32>(texture->format->vkFormat)};
        const auto guestFormat{static_cast<u32>(texture->guest->format->vkFormat)};
        const auto writeSerial{texture->gpuWriteSerial};
        const auto captureCycle{cycle};
        const auto recorded{std::make_shared<std::atomic<bool>>(false)};
        captureCycle->AttachObjects(staging, texture);

        const vk::ImageSubresourceRange range{
            vk::ImageAspectFlagBits::eColor, mip, 1, layer, 1
        };
        const vk::BufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, layer, 1},
            .imageOffset = {static_cast<i32>(x), static_cast<i32>(y), 0},
            .imageExtent = {roiWidth, roiHeight, 1}
        };
        auto recorder{[image, range, copy, staging, recorded, bytes]
            (vk::raii::CommandBuffer &commands, const std::shared_ptr<FenceCycle> &, GPU &) {
            const vk::ImageMemoryBarrier before{
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = range,
            };
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
            commands.copyImageToBuffer(image, vk::ImageLayout::eGeneral, staging->vkBuffer,
                vk::ArrayProxy<const vk::BufferImageCopy>(1, &copy));
            auto after{before};
            after.srcAccessMask = vk::AccessFlagBits::eTransferRead;
            after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost, {}, {},
                vk::BufferMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eHostRead,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = staging->vkBuffer, .offset = 0, .size = bytes,
                }, {});
            recorded->store(true, std::memory_order_release);
        }};

        // At pass start, no render pass is active; recording a pre-pass copy
        // cannot split one. The post-pass image copy is queued until the
        // renderer's OWN natural render-pass boundary; no forced Finish/Submit.
        if (postNaturalPass) AddPostRpCommand(std::move(recorder));
        else AddOutsideRpCommand(std::move(recorder));

        texture::diagnostic::SpongeBobMiniInfo info{
            .operation = operation, .role = role,
            .image = std::bit_cast<u64>(static_cast<VkImage>(image)),
            .generation = generation, .writeSerial = writeSerial,
            .hostFormat = hostFormat, .guestFormat = guestFormat,
            .width = width, .height = height, .x = x, .y = y,
            .regionWidth = roiWidth, .regionHeight = roiHeight,
            .mip = mip, .layer = layer, .blockWidth = blockWidth,
            .blockHeight = blockHeight, .bytesPerBlock = texture->format->bpb,
            .size = static_cast<u32>(bytes),
            .pipelineHash = draw.pipelineHash, .channelSequence = draw.channelSequence,
            .shaderHashes = draw.shaderHashes,
            .drawKind = draw.kind, .topology = draw.topology,
            .indexed = draw.indexed, .first = draw.first,
            .count = draw.count, .instances = draw.instances,
            .sampledOrdinal = sampledOrdinal,
            .ticIndex = sampledOrdinal < draw.sampledBindings.size()
                ? draw.sampledBindings[sampledOrdinal].ticIndex : UINT32_MAX,
            .matchingTics = sampledOrdinal < draw.sampledBindings.size()
                ? draw.sampledBindings[sampledOrdinal].matchingEntries : 0,
            .ticIova = sampledOrdinal < draw.sampledBindings.size()
                ? draw.sampledBindings[sampledOrdinal].iova : 0,
        };
        gpu.spongeBobMini.Add(info, [captureCycle, staging, recorded, bytes] {
            // Avoid waiting on an unsubmitted slot if the user exports before
            // the current render pass/command executor is flushed.
            if (captureCycle->IsCancelled() ||
                !recorded->load(std::memory_order_acquire) ||
                !captureCycle->Poll(false))
                return std::vector<u8>{};
            captureCycle->Wait(); // Already signalled; Android IO worker only.
            if (captureCycle->IsCancelled()) return std::vector<u8>{};
            return std::vector<u8>(staging->data(), staging->data() + bytes);
        });
    }

    void CommandExecutor::AddSubpass(SubpassFunction &&function, vk::Rect2D renderArea, span<TextureView *> sampledImages, span<TextureView *> inputAttachments, span<TextureView *> colorAttachments, TextureView *depthStencilAttachment, bool noSubpassCreation, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask, SubpassHookFactory hookFactory, span<TextureView *> storageWrites, span<TextureView *> storageReads, const texture::diagnostic::SpongeBobMiniDrawInfo *sbDrawInfo) {
        // An explicit Android UI action arms one operation; normal draws only
        // perform the cheap atomic check. Never scan or capture while disarmed.
        TextureView *sbTarget{};
        boost::container::small_vector<TextureView *, 4> sbInputs;
        boost::container::small_vector<u32, 4> sbInputOrdinals;
        // Do not split an already-open render pass to start a diagnostic.
        // Wait for the next naturally empty render-pass slot.
        if (sbDrawInfo && gpu.spongeBobMini.Armed() && renderPass == nullptr) {
            for (auto *candidate : colorAttachments)
                if (SbMiniEligible(candidate, true)) { sbTarget = candidate; break; }
            if (sbTarget) {
                // Keep the small capture budget useful: retain substantial
                // sampled images first, then fill free slots with tiny inputs.
                // The original descriptor ordinal stays attached to each ROI.
                const auto substantial{[](TextureView *view) {
                    const auto &texture{*view->texture};
                    return std::max(1U, texture.dimensions.width >> view->range.baseMipLevel) >= 16 ||
                           std::max(1U, texture.dimensions.height >> view->range.baseMipLevel) >= 16;
                }};
                for (bool preferLarge : {true, false}) {
                    for (u32 ordinal{}; ordinal < sampledImages.size() && sbInputs.size() < 4; ++ordinal) {
                        auto *candidate{sampledImages[ordinal]};
                        if (SbMiniEligible(candidate, false) &&
                            substantial(candidate) == preferLarge) {
                            sbInputs.push_back(candidate);
                            sbInputOrdinals.push_back(ordinal);
                        }
                    }
                    if (sbInputs.size() == 4) break;
                }
            }
        }
        // The title-screen trace repeatedly caught compositing passes that
        // sample only a 1x1 white texel and never modify the measured ROI.
        // In preferred scene mode, skip these overlays while retaining the
        // original broad selection as an explicit fallback. HDR color outputs
        // (VK_FORMAT_B10G11R11_UFLOAT_PACK32) remain eligible even when their
        // sampled inputs are small; resource producers can use solid inputs.
        if (sbTarget && !sbInputs.empty() && gpu.spongeBobMini.PreferScenePass()) {
            const bool hdrTarget{
                sbTarget->texture->format->vkFormat == vk::Format::eB10G11R11UfloatPack32
            };
            const bool hasSubstantialInput{std::any_of(sbInputs.begin(), sbInputs.end(),
                [](TextureView *view) {
                    const auto &texture{*view->texture};
                    return std::max(1U, texture.dimensions.width >> view->range.baseMipLevel) >= 16 ||
                           std::max(1U, texture.dimensions.height >> view->range.baseMipLevel) >= 16;
                })};
            if (!hdrTarget && !hasSubstantialInput) {
                const auto skipped{gpu.spongeBobMini.SkippedOverlayPass()};
                if (skipped <= 12 || (skipped & (skipped - 1)) == 0) {
                    LOGI("[SB_MINI] skipped_overlay_pass={} target_fmt={} input_count={} reason=no_substantial_sampled_images",
                        skipped, static_cast<u32>(sbTarget->texture->format->vkFormat),
                        sbInputs.size());
                }
                sbTarget = nullptr;
            }
        }
        u64 sbOperation{};
        if (sbTarget && !sbInputs.empty()) {
            // Attach before allocating snapshots. Existing alias reconciliation
            // exceptions must propagate into the executor's regular retry path.
            AttachTexture(sbTarget);
            for (auto *candidate : sbInputs) AttachTexture(candidate);
            sbOperation = gpu.spongeBobMini.Begin();
            if (sbOperation) {
                sbNaturalPassOperation = sbOperation;
                sbNaturalPassDraws = 0;
                LOGI("[SB_DRAW] op={} pipeline={:016X} seq={} kind={} topology={} indexed={} first={} count={} instances={} shader0={:016X} shader1={:016X} shader2={:016X} shader3={:016X} shader4={:016X} shader5={:016X}",
                    sbOperation, sbDrawInfo->pipelineHash, sbDrawInfo->channelSequence,
                    sbDrawInfo->kind, sbDrawInfo->topology, sbDrawInfo->indexed,
                    sbDrawInfo->first, sbDrawInfo->count, sbDrawInfo->instances,
                    sbDrawInfo->shaderHashes[0], sbDrawInfo->shaderHashes[1],
                    sbDrawInfo->shaderHashes[2], sbDrawInfo->shaderHashes[3],
                    sbDrawInfo->shaderHashes[4], sbDrawInfo->shaderHashes[5]);
                LOGI("[SB_MINI] selected operation={} target={} dimensions={}x{} input_count={} format={}",
                    sbOperation, static_cast<const void *>(sbTarget->texture.get()),
                    sbTarget->texture->dimensions.width, sbTarget->texture->dimensions.height,
                    sbInputs.size(), static_cast<int>(sbTarget->texture->format->vkFormat));
                try {
                    QueueSbMiniImage(sbTarget, sbOperation, 1, *sbDrawInfo, UINT32_MAX);
                    for (size_t i{}; i < sbInputs.size(); ++i) {
                        const auto ordinal{sbInputOrdinals[i]};
                        const auto &binding{sbDrawInfo->sampledBindings[ordinal]};
                        LOGI("[SB_BIND] op={} role={} ordinal={} tic={} iova={:X} matches={} format={}",
                            sbOperation, i + 3, ordinal, binding.ticIndex,
                            binding.iova, binding.matchingEntries,
                            static_cast<u32>(sbInputs[i]->format->vkFormat));
                        QueueSbMiniImage(sbInputs[i], sbOperation, static_cast<u32>(i + 3), *sbDrawInfo, ordinal);
                    }
                } catch (const std::exception &e) {
                    LOGW("[SB_MINI] before capture unavailable: {}", e.what());
                }
            }
        }
        PreflightTextureUses(storageReads, {});
        bool gotoNext{CreateRenderPassWithSubpass(renderArea, sampledImages, inputAttachments, colorAttachments, depthStencilAttachment ? &*depthStencilAttachment : nullptr, noSubpassCreation, srcStageMask, dstStageMask, storageWrites)};

        SubpassHooks hooks{};
        if (hookFactory)
            hooks = hookFactory(renderPassIndex);

        if (gotoNext) {
            if (hooks.before) {
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassFunctionNode>(), std::move(hooks.before));
                slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), std::forward<decltype(function)>(function));
            } else {
                slot->nodes.emplace_back(std::in_place_type_t<node::NextSubpassFunctionNode>(), std::forward<decltype(function)>(function));
            }
        } else {
            if (hooks.before)
                slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), std::move(hooks.before));
            slot->nodes.emplace_back(std::in_place_type_t<node::SubpassFunctionNode>(), std::forward<decltype(function)>(function));
        }

        if (sbOperation) {
            try {
                // The RP remains open: the copy is deferred after the
                // *natural* RP-end node, so later draws are included.
                QueueSbMiniImage(sbTarget, sbOperation, 2, *sbDrawInfo, UINT32_MAX, true);
            } catch (const std::exception &e) {
                LOGW("[SB_MINI] post-natural-pass capture unavailable: {}", e.what());
                throw;
            }
        }
        if (sbNaturalPassOperation) {
            const auto sequence{++sbNaturalPassDraws};
            if (sequence <= 128 || (sequence & (sequence - 1)) == 0) {
                if (sbDrawInfo) {
                    LOGI("[SB_PASS_DRAW] op={} ordinal={} rp={} pipeline={:016X} shaderVS={:016X} shaderFS={:016X} kind={} indexed={} first={} count={} instances={} scissor={},{},{},{} conditional={} depthTest={}",
                        sbNaturalPassOperation, sequence, renderPassIndex,
                        sbDrawInfo->pipelineHash, sbDrawInfo->shaderHashes[1],
                        sbDrawInfo->shaderHashes[5], sbDrawInfo->kind, sbDrawInfo->indexed,
                        sbDrawInfo->first, sbDrawInfo->count, sbDrawInfo->instances,
                        sbDrawInfo->scissorX, sbDrawInfo->scissorY,
                        sbDrawInfo->scissorWidth, sbDrawInfo->scissorHeight,
                        sbDrawInfo->conditional, sbDrawInfo->depthTest);
                } else {
                    LOGI("[SB_PASS_NON_SHADER] op={} ordinal={} rp={} reason=subpass_without_draw_metadata",
                        sbNaturalPassOperation, sequence, renderPassIndex);
                }
            }
        }
        if (slot->nodes.size() > *state.settings->executorFlushThreshold && !gotoNext)
            Submit();
    }

    void CommandExecutor::AddOutsideRpCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        if (renderPass)
            FinishRenderPass();

        slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::BreakRenderPass() {
        FinishRenderPass();
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

    void CommandExecutor::AddPostRpCommand(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> &&function) {
        if (renderPass)
            slot->pendingPostRenderPassNodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
        else
            slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(), std::forward<decltype(function)>(function));
    }

    void CommandExecutor::AddFullBarrier() {
        AddOutsideRpCommand([](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &) {
            RecordFullBarrier(commandBuffer);
        });
    }

    void CommandExecutor::AddClearColorSubpass(TextureView *attachment, const vk::ClearColorValue &value) {
        bool gotoNext{CreateRenderPassWithSubpass(vk::Rect2D{.extent = attachment->texture->dimensions}, {}, {}, attachment, nullptr)};
        if (sbNaturalPassOperation) {
            const auto ordinal{++sbNaturalPassDraws};
            if (ordinal <= 128 || (ordinal & (ordinal - 1)) == 0)
                LOGI("[SB_PASS_CLEAR] op={} ordinal={} rp={} kind=color",
                    sbNaturalPassOperation, ordinal, renderPassIndex);
        }
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
        if (sbNaturalPassOperation) {
            const auto ordinal{++sbNaturalPassDraws};
            if (ordinal <= 128 || (ordinal & (ordinal - 1)) == 0)
                LOGI("[SB_PASS_CLEAR] op={} ordinal={} rp={} kind=depth_stencil",
                    sbNaturalPassOperation, ordinal, renderPassIndex);
        }
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

    void CommandExecutor::AddSubpassBoundaryCallback(std::function<void()> &&callback) {
        subpassBoundaryCallbacks.emplace_back(std::move(callback));
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
                // SynchronizeHostInline may replace the previous cycle with this
                // cycle for CPU-dirty staged uploads, so order that predecessor first.
                const auto previousCycle{texture->cycle};
                if (ranges::find(chainedCycles, previousCycle.get()) == chainedCycles.end()) {
                    // Chaining fence completion does not order Vulkan queue submissions
                    // from independent command-record threads. Ensure the previous
                    // writer has reached vkQueueSubmit before submitting this consumer.
                    // WaitSubmit is CPU submission-only: it never waits for GPU completion.
                    if (previousCycle && previousCycle != cycle)
                        previousCycle->WaitSubmit();
                    cycle->ChainCycle(previousCycle);
                    chainedCycles.emplace_back(previousCycle.get());
                }

                texture->SynchronizeHostInline(slot->commandBuffer, cycle, true);
                // Retain any lifetime dependency introduced during synchronization.
                // A TextureView already keeps the texture itself alive.
                if (texture->cycle != previousCycle)
                    cycle->ChainCycle(texture->cycle);
                texture->cycle = cycle;
                texture->pendingGpuWriteTag.store({}, std::memory_order_relaxed);
                texture->pendingRegionUseTag = {};
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
        regionOperationUses.clear();
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
            SubmitInternal();
            submissionNumber++;
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

    void CommandExecutor::HandleTextureBackingPromotion(const TextureBackingPromotionRequest &request) {
        // Restart preparation after submitting old nodes. In particular, allocator
        // pointers and descriptors from the interrupted operation must not be reused.
        Submit();
        if (request.levels > request.texture->levelCount && request.texture->regionTracked.load(std::memory_order_relaxed))
            Texture::DrainRegionAuthority({request.texture}, tag);
        ContextLock lock{tag, *request.texture};
        request.texture->PromoteBackingForView(request.format, request.levels);
    }

    void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &request) {
        Submit();
        if (request.lookupManager) {
            // Complete registration/drain and cache the requested image before
            // replaying preparation, which may immediately reattach its peers.
            request.lookupManager->FindOrCreate(*request.lookupLayout);
            return;
        }
        TextureManager::SynchronizeGuestOverlaps(request.overlapReadbacks);
        if (!request.registration.empty())
            Texture::RegisterAliases(request.registration, request.registrationLayout.get());
        if (request.target)
            request.target->ReconcileAliases();
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
