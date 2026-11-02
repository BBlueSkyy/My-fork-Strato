// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

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
#include <gpu/diagnostic/pacman_triangle_capture.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <nce.h>

namespace skyline::gpu::interconnect {
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
                if (slot->capture && !renderDocApi)
                    LOGI("MINIRD_PACMAN RenderDoc layer not loaded; text trace remains available");
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


    void CommandExecutor::CapturePacmanSceneStep(TextureView *target, u32 ordinal,
                                                  std::string_view drawDescription) {
        auto &cap{diagnostic::PacmanCapture()};
        if (!cap.SnapshotsEnabled() || !target || !target->texture)
            return;
        auto tex{target->texture};
        // The first capture established that the scene color pass is 1280x720.
        // Check dimensions and image-copy prerequisites, never alter the RT.
        if (tex->dimensions.width != 1280 || tex->dimensions.height != 720 ||
            tex->dimensions.depth != 1 || tex->levelCount != 1 ||
            tex->layerCount != 1 || tex->sampleCount != vk::SampleCountFlagBits::e1 ||
            !tex->surfaceSize || !tex->format ||
            !(tex->format->vkAspect & vk::ImageAspectFlagBits::eColor) ||
            tex->format->IsCompressed() ||
            !(tex->usage & vk::ImageUsageFlagBits::eTransferSrc) ||
            (tex->layout != vk::ImageLayout::eGeneral &&
             tex->layout != vk::ImageLayout::eTransferSrcOptimal)) {
            cap.Record(fmt::format(
                "SCENE_SNAPSHOT_SKIPPED ordinal={} tex={} layout={} usage={} size={}",
                ordinal, fmt::ptr(tex.get()), vk::to_string(tex->layout),
                static_cast<u32>(tex->usage), tex->surfaceSize));
            return;
        }
        if (cap.ScanEnabled()) {
            if (tex->format->vkFormat != vk::Format::eB10G11R11UfloatPack32)
                return; // Raw packed-float scan is only valid for this format.
            const auto ticket{cap.ReserveScan(tex->surfaceSize)};
            if (!ticket) return;
            const auto [scanId, epoch]{*ticket};
            const auto interval{cap.PresentedFrames()};
            const auto folder{cap.CaptureDirectory()};
            BreakRenderPass(); // Explicitly armed: never split a normal draw.
            auto staging{gpu.memory.AllocateStagingBuffer(tex->surfaceSize)};
            AddCommand([tex, staging](vk::raii::CommandBuffer &cmd,
                                       const std::shared_ptr<FenceCycle> &cycle, GPU &) {
                cycle->AttachObjects(tex, staging);
                tex->CopyIntoStagingBuffer(cmd, staging);
            });
            pacmanSceneReadbacks.emplace_back(
                [staging, folder, scanId, epoch, interval, ordinal] {
                    // VK_FORMAT_B10G11R11_UFLOAT_PACK32: threshold >100
                    // corresponds to packed R/G11 >1380, B10 >690.
                    // The threshold separates prior bad (>10k pixels)
                    // and good (zero pixels) captures. Detection only;
                    // no shader or rendering decisions depend on it.
                    auto *bytes{static_cast<const u8 *>(staging->data())};
                    std::uint32_t above100{};
                    for (std::size_t i{}; i < 1280ULL * 720; ++i) {
                        std::uint32_t word{};
                        std::memcpy(&word, bytes + i * 4, sizeof(word));
                        if ((word & 0x7FFU) > 1380U ||
                            ((word >> 11) & 0x7FFU) > 1380U ||
                            ((word >> 22) & 0x3FFU) > 690U)
                            ++above100;
                    }
                    auto &capture{diagnostic::PacmanCapture()};
                    const bool keep{capture.KeepScanImage(epoch, above100)};
                    bool ok{true};
                    if (keep) {
                        const auto stem{fmt::format("hdr_scan_{:02}_i{}", scanId, interval)};
                        const auto rawPath{folder / (stem + ".raw")};
                        const auto metaPath{folder / (stem + ".txt")};
                        std::ofstream raw{rawPath, std::ios::binary | std::ios::trunc};
                        if (raw)
                            raw.write(reinterpret_cast<const char *>(bytes),
                                      static_cast<std::streamsize>(staging->size()));
                        raw.close();
                        std::ofstream meta{metaPath, std::ios::trunc};
                        if (meta)
                            meta << "interval=" << interval << " scan_id=" << scanId
                                 << " draw_ordinal=" << ordinal
                                 << " format=B10G11R11UfloatPack32"
                                 << " width=1280 height=720 over_100_pixels=" << above100
                                 << " threshold_pixels=512\n";
                        meta.close();
                        ok = raw.good() && meta.good();
                    }
                    capture.ScanFinished(epoch, scanId, above100, ok);
                    if (above100 >= 512)
                        LOGI("MINIRD_PACMAN HDR scan match sample={} pixels_above_100={} saved={}",
                             scanId, above100, keep && ok);
                });
            return;
        }
        const auto idOpt{cap.ReserveSnapshot(tex->surfaceSize)};
        if (!idOpt)
            return;
        const auto id{*idOpt};
        const auto interval{cap.PresentedFrames()};
        const auto folder{cap.CaptureDirectory()};
        const auto rawPath{folder / fmt::format(
            "scene_{:02}_i{}_draw{:03}_1280x720.raw", id, interval, ordinal)};
        const auto metaPath{folder / fmt::format("scene_{:02}.txt", id)};
        const auto description{std::string{drawDescription}};
        const auto details{fmt::format(
            "id={} interval={} scene_draw_ordinal={} texture={} "
            "dimensions=1280x720 format={} layout={} size={} "
            "executor={} submission={} draw={} note=post_draw_gpu_ordered\n",
            id, interval, ordinal, fmt::ptr(tex.get()),
            vk::to_string(tex->format->vkFormat), vk::to_string(tex->layout),
            tex->surfaceSize, fmt::ptr(this), submissionNumber, description)};

        // Ends the current render pass, then copies the target. This is a
        // deliberate diagnostic-only split, never performed in normal play.
        BreakRenderPass();
        auto staging{gpu.memory.AllocateStagingBuffer(tex->surfaceSize)};
        AddCommand([tex, staging](vk::raii::CommandBuffer &cmd,
                                   const std::shared_ptr<FenceCycle> &cycle, GPU &) {
            cycle->AttachObjects(tex, staging);
            tex->CopyIntoStagingBuffer(cmd, staging);
        });
        pacmanSceneReadbacks.emplace_back([staging, rawPath, metaPath, details, id] {
            std::ofstream out{rawPath, std::ios::binary | std::ios::trunc};
            if (out)
                out.write(reinterpret_cast<const char *>(staging->data()),
                          static_cast<std::streamsize>(staging->size()));
            out.close();
            const bool rawGood{out.good()};
            std::ofstream info{metaPath, std::ios::trunc};
            if (info)
                info << details;
            info.close();
            const bool ok{rawGood && info.good()};
            diagnostic::PacmanCapture().SnapshotFinished(ok);
            if (ok)
                LOGI("MINIRD_PACMAN scene snapshot {} written", id);
            else
                LOGE("MINIRD_PACMAN scene snapshot {} failed", id);
        });
        cap.Record(fmt::format(
            "SCENE_SNAPSHOT_RESERVED id={} interval={} ordinal={} tex={} bytes={}",
            id, interval, ordinal, fmt::ptr(tex.get()), tex->surfaceSize));
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
        // One operation cannot define an order for two distinct writers of
        // the same guest bytes. Multiple views of one backing remain valid.
        boost::container::small_vector<TextureView *, 8> writers;
        for (auto view : outputAttachmentViews) if (view) writers.push_back(view);
        Texture::ValidateAliasWrites(writers);
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
                for (auto &callback : subpassBoundaryCallbacks)
                    callback();

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
            for (auto &callback : subpassBoundaryCallbacks)
                callback();

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

        return gotoNext;
    }

    void CommandExecutor::FinishRenderPass() {
        if (renderPass) {
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

        if (view->texture->HasAliases())
            view->texture->SynchronizeHost(false); // Initialize before claiming a GPU write, once per generation.
        return didLock;
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

    void CommandExecutor::AddSubpass(SubpassFunction &&function, vk::Rect2D renderArea, span<TextureView *> sampledImages, span<TextureView *> inputAttachments, span<TextureView *> colorAttachments, TextureView *depthStencilAttachment, bool noSubpassCreation, vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask, SubpassHookFactory hookFactory) {
        bool gotoNext{CreateRenderPassWithSubpass(renderArea, sampledImages, inputAttachments, colorAttachments, depthStencilAttachment ? &*depthStencilAttachment : nullptr, noSubpassCreation, srcStageMask, dstStageMask)};

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

        auto &pacmanDiagnostic{diagnostic::PacmanCapture()};
        const auto testMode{pacmanDiagnostic.ExecutorExperimentMode()};
        // One Maxwell3D draw signature is consumed for every AddSubpass;
        // this prevents accidental association with subsequent engine work.
        const auto drawSignature{std::exchange(pacmanNextDrawSignature, std::string{})};
        bool sceneSubmit{}, callbackRefresh{}, hasColor{}, hdrScene{}, viOutput{};
        const bool postProbeMode{testMode >= 7 && testMode <= 16};
        if (testMode >= 3 && testMode <= 16) {
            for (auto *view : colorAttachments) {
                if (!view || !view->texture || !view->texture->format)
                    continue;
                auto &t{*view->texture};
                hasColor = true;
                const auto w{t.dimensions.width}, h{t.dimensions.height};
                if (w == 1280 && h == 720) {
                    if (testMode == 3) sceneSubmit = true;
                    if (t.format->vkFormat == vk::Format::eB10G11R11UfloatPack32)
                        hdrScene = true;
                }
                if (w == 1920 && h == 1080)
                    viOutput = true;
            }
            if (testMode >= 4) {
                const bool depthOnly{
                    !hasColor && depthStencilAttachment && depthStencilAttachment->texture &&
                    depthStencilAttachment->texture->dimensions.width == 2048 &&
                    depthStencilAttachment->texture->dimensions.height == 2048
                };
                if (depthOnly) {
                    pacmanSawDepthPass = true;
                    pacmanSawHdrScenePass = false;
                }
                if (hdrScene) {
                    for (auto *view : colorAttachments)
                        if (view && view->texture && view->texture->format &&
                            view->texture->dimensions.width == 1280 &&
                            view->texture->dimensions.height == 720 &&
                            view->texture->format->vkFormat ==
                                vk::Format::eB10G11R11UfloatPack32) {
                            pacmanLastHdrProducerIdentity =
                                reinterpret_cast<std::uintptr_t>(view->texture.get());
                            break;
                        }
                    if (testMode == 4 && pacmanSawDepthPass)
                        sceneSubmit = true;
                    pacmanSawDepthPass = false;
                    pacmanSawHdrScenePass = true;
                } else if (hasColor && pacmanSawHdrScenePass) {
                    if (testMode == 5 && !viOutput)
                        sceneSubmit = true;
                    if (testMode == 6 && viOutput)
                        sceneSubmit = true;
                    if (postProbeMode && !viOutput) {
                        // The successful broad mode 5 submitted ~four times
                        // per host presentation. These slots are diagnostic,
                        // not a claim that host presents equal guest frames.
                        const auto candidate{++pacmanPostCandidateSerial};
                        const auto slotIndex{static_cast<u32>((candidate - 1) % 4) + 1};
                        sceneSubmit = testMode == 7 ||
                                      (testMode >= 8 && testMode <= 11 &&
                                       slotIndex == testMode - 7) ||
                                      (testMode == 13 && (slotIndex == 1 || slotIndex == 3)) ||
                                      (testMode == 14 && (slotIndex == 2 || slotIndex == 3)) ||
                                      (testMode == 15 && (slotIndex == 3 || slotIndex == 4));
                        callbackRefresh = testMode == 16 && (slotIndex == 3 || slotIndex == 4);
                        if (pacmanDiagnostic.Active()) {
                            // Observe only the inputs/outputs of the two narrowed
                            // candidates. Store identities, never extend lifetimes
                            // or dereference an identity after its object is gone.
                            if (slotIndex == 3 || slotIndex == 4) {
                                const auto watch{[&](TextureView *view) {
                                    if (!view || !view->texture) return;
                                    const auto identity{reinterpret_cast<std::uintptr_t>(view->texture.get())};
                                    for (u32 i{}; i < pacmanTrackedTextureCount; ++i)
                                        if (pacmanTrackedTextures[i] == identity) return;
                                    if (pacmanTrackedTextureCount < pacmanTrackedTextures.size())
                                        pacmanTrackedTextures[pacmanTrackedTextureCount++] = identity;
                                    else
                                        pacmanDiagnostic.Record("TEXTURE_TRACK_LIMIT capacity=8");
                                }};
                                for (auto *view : sampledImages) watch(view);
                                for (auto *view : colorAttachments) watch(view);
                            }
                            std::string targets, inputs;
                            bool sampledSameHdrIdentity{};
                            u32 n{};
                            for (auto *view : colorAttachments) {
                                if (n++ >= 4) break;
                                if (view && view->texture && view->texture->format) {
                                    auto &t{*view->texture};
                                    targets += fmt::format(
                                        "[tex={} {}x{} fmt={} layout={} cycle={}]",
                                        fmt::ptr(view->texture.get()), t.dimensions.width,
                                        t.dimensions.height, static_cast<u32>(t.format->vkFormat),
                                        static_cast<u32>(t.layout), fmt::ptr(t.cycle.get()));
                                } else targets += "[null]";
                            }
                            n = 0;
                            for (auto *view : sampledImages) {
                                if (n++ >= 8) break;
                                if (view && view->texture && view->texture->format) {
                                    auto &t{*view->texture};
                                    if (reinterpret_cast<std::uintptr_t>(view->texture.get()) ==
                                        pacmanLastHdrProducerIdentity)
                                        sampledSameHdrIdentity = true;
                                    inputs += fmt::format(
                                        "[tex={} {}x{} fmt={} layout={} cycle={}]",
                                        fmt::ptr(view->texture.get()), t.dimensions.width,
                                        t.dimensions.height, static_cast<u32>(t.format->vkFormat),
                                        static_cast<u32>(t.layout), fmt::ptr(t.cycle.get()));
                                } else inputs += "[null]";
                            }
                            pacmanDiagnostic.Record(fmt::format(
                                "POST_CANDIDATE ordinal={} slot={} mode={} selected={} "
                                "goto_next={} natural_flush={} present_interval={} "
                                "exec={} sub={} rp={} own_cycle={} colors={} "
                                "sampled_count={} sampled_first8={} input_attachments={} "
                                "targets={} last_hdr_texture=0x{:X} sampled_same_hdr={} callback_refresh={} draw={}",
                                candidate, slotIndex, testMode, sceneSubmit || callbackRefresh, gotoNext,
                                slot->nodes.size() > *state.settings->executorFlushThreshold,
                                pacmanDiagnostic.PresentedFrames(), fmt::ptr(this),
                                submissionNumber, renderPassIndex, fmt::ptr(cycle.get()),
                                colorAttachments.size(), sampledImages.size(), inputs,
                                inputAttachments.size(), targets,
                                pacmanLastHdrProducerIdentity, sampledSameHdrIdentity, callbackRefresh,
                                drawSignature));
                        }
                    }
                    // The event has been consumed even if its slot was not
                    // selected, preventing repeated submissions for one draw.
                    if (sceneSubmit || testMode == 5 || postProbeMode)
                        pacmanSawHdrScenePass = false;
                }
            }
        }
        bool naturalFlush{slot->nodes.size() > *state.settings->executorFlushThreshold};
        if (!gotoNext && callbackRefresh && !pacmanFlushingCallbacks) {
            // Differential experiment: execute precisely Submit's callback
            // bundle, retaining the current slot, resources and execution tag.
            // Engine callbacks can publish deferred work: guard reentry and
            // report the surrounding state instead of calling this a pure
            // descriptor refresh or assuming the command list stays identical.
            const auto callbackExecutionTag{executionTag};
            const auto traceCallbacks{[&](std::string_view phase) {
                if (pacmanDiagnostic.Active())
                    pacmanDiagnostic.Record(fmt::format(
                        "EXECUTOR_CALLBACKS phase={} candidate={} exec={} sub={} rp={} "
                        "cycle={} execution_tag_unchanged={} nodes={} pending_post_rp={} callbacks={} "
                        "natural_flush={} threshold={}",
                        phase, pacmanPostCandidateSerial, fmt::ptr(this), submissionNumber,
                        renderPassIndex, fmt::ptr(cycle.get()), executionTag == callbackExecutionTag,
                        slot->nodes.size(), slot->pendingPostRenderPassNodes.size(),
                        flushCallbacks.size(), naturalFlush, *state.settings->executorFlushThreshold));
            }};
            traceCallbacks("before");
            pacmanFlushingCallbacks = true;
            try {
                for (const auto &flushCallback : flushCallbacks)
                    flushCallback();
            } catch (...) {
                pacmanFlushingCallbacks = false;
                throw;
            }
            pacmanFlushingCallbacks = false;
            // Callback-published work remains in this slot and must count
            // toward the ordinary threshold once the callback bundle exits.
            naturalFlush = slot->nodes.size() > *state.settings->executorFlushThreshold;
            pacmanDiagnostic.ExecutorCallbackStep();
            traceCallbacks("after");
        }
        if (!gotoNext && naturalFlush && pacmanFlushingCallbacks && pacmanDiagnostic.Active())
            pacmanDiagnostic.Record(fmt::format(
                "EXECUTOR_CALLBACK_DEFERRED_NATURAL_SUBMIT exec={} candidate={} nodes={} threshold={}",
                fmt::ptr(this), pacmanPostCandidateSerial, slot->nodes.size(),
                *state.settings->executorFlushThreshold));
        // In callback-only mode a callback can itself enqueue a draw. Let
        // the outer invocation finish its bundle before threshold submission.
        if (!gotoNext && !pacmanFlushingCallbacks && (naturalFlush || sceneSubmit)) {
            if (testMode == 16 && naturalFlush && pacmanDiagnostic.Active())
                pacmanDiagnostic.Record(fmt::format(
                    "EXECUTOR_NATURAL_SUBMIT exec={} sub={} candidate={} nodes={} threshold={}",
                    fmt::ptr(this), submissionNumber, pacmanPostCandidateSerial,
                    slot->nodes.size(), *state.settings->executorFlushThreshold));
            if (sceneSubmit) {
                pacmanDiagnostic.ExecutorExperimentStep();
                if (pacmanDiagnostic.Active())
                    pacmanDiagnostic.Record(fmt::format(
                        "EXECUTOR_BOUNDARY_SUBMIT mode={} natural_flush={} submit={} "
                        "hdr={} vi={} color={} post_candidate={}",
                        testMode, naturalFlush, submissionNumber, hdrScene, viOutput,
                        hasColor, pacmanPostCandidateSerial));
            }
            Submit();
        } else if (gotoNext && sceneSubmit && pacmanDiagnostic.Active()) {
            pacmanDiagnostic.Record(fmt::format(
                "EXECUTOR_BOUNDARY_SKIPPED mode={} reason=next_subpass", testMode));
        }

        // Mode 1: end each renderpass without submitting.
        // Mode 2: same split plus an all-commands Vulkan memory barrier.
        if (testMode == 1) {
            BreakRenderPass();
            pacmanDiagnostic.ExecutorExperimentStep();
        } else if (testMode == 2) {
            AddFullBarrier();
            pacmanDiagnostic.ExecutorExperimentStep();
        }
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

    void CommandExecutor::TracePacmanTextureSync(Texture *texture, std::string_view phase) {
        auto &capture{diagnostic::PacmanCapture()};
        if (!pacmanTrackedTextureCount || !capture.Active())
            return;
        const auto identity{reinterpret_cast<std::uintptr_t>(texture)};
        bool tracked{};
        for (u32 index{}; index < pacmanTrackedTextureCount && index < pacmanTrackedTextures.size(); ++index)
            tracked |= pacmanTrackedTextures[index] == identity;
        if (!tracked)
            return;

        // The executor already owns this live texture's resource lock. Trap
        // callbacks can still change dirty state, so copy that state briefly
        // under its own mutex and release it before formatting or recording.
        u32 dirtyState{};
        bool memoryFreed{};
        u64 writeSerial{}, readbackSerial{};
        {
            std::scoped_lock lock{texture->stateMutex};
            dirtyState = static_cast<u32>(texture->dirtyState);
            memoryFreed = texture->memoryFreed;
            writeSerial = texture->gpuWriteSerial;
            readbackSerial = texture->gpuReadbackSerial;
        }
        const auto alias{texture->aliasGroup.load()};
        std::string mappings;
        size_t mappingCount{};
        if (texture->guest) {
            mappingCount = texture->guest->mappings.size();
            for (size_t index{}; index < mappingCount && index < 4; ++index) {
                const auto mapping{texture->guest->mappings[index]};
                mappings += fmt::format("[ptr={} bytes={}]", fmt::ptr(mapping.data()), mapping.size());
            }
        }
        capture.Record(fmt::format(
            "TEXTURE_SUBMIT_SYNC phase={} exec={} sub={} tex={} image={} generation={} "
            "cycle={} own_cycle={} dimensions={}x{}x{} fmt={} layout={} "
            "dirty={} memory_freed={} write_serial={} readback_serial={} "
            "alias_group={} alias_stale={} last_rp={} last_usage={} "
            "pending_stages={} read_stages={} mapping_count={} mappings_first4={}",
            phase, fmt::ptr(this), submissionNumber, fmt::ptr(texture),
            fmt::ptr(static_cast<VkImage>(texture->GetBacking())), texture->backingGeneration,
            fmt::ptr(texture->cycle.get()), fmt::ptr(cycle.get()),
            texture->dimensions.width, texture->dimensions.height, texture->dimensions.depth,
            static_cast<u32>(texture->format->vkFormat), static_cast<u32>(texture->layout),
            dirtyState, memoryFreed, writeSerial, readbackSerial,
            fmt::ptr(alias.get()), texture->aliasStale.load(), texture->lastRenderPassIndex,
            static_cast<u32>(texture->lastRenderPassUsage),
            static_cast<VkPipelineStageFlags>(texture->pendingStageMask),
            static_cast<VkPipelineStageFlags>(texture->readStageMask), mappingCount, mappings));
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

                const bool traceTextureSync{pacmanTrackedTextureCount && diagnostic::PacmanCapture().Active()};
                if (traceTextureSync)
                    TracePacmanTextureSync(texture.operator->(), "before");
                texture->SynchronizeHostInline(slot->commandBuffer, cycle, true);
                if (traceTextureSync) {
                    // Only the staged inline upload changes the texture's cycle
                    // here. A pre-existing self cycle makes that inference ambiguous.
                    const auto phase{previousCycle == cycle ? "after_inline_upload_unknown" :
                        texture->cycle == cycle ? "after_inline_upload" : "after_no_inline_upload"};
                    TracePacmanTextureSync(texture.operator->(), phase);
                }
                // Retain any lifetime dependency introduced during synchronization.
                // A TextureView already keeps the texture itself alive.
                if (texture->cycle != previousCycle)
                    cycle->ChainCycle(texture->cycle);
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
        if (pacmanFlushingCallbacks && diagnostic::PacmanCapture().Active())
            diagnostic::PacmanCapture().Record(fmt::format(
                "EXECUTOR_CALLBACK_REENTRANT_SUBMIT exec={} sub={} nodes={}",
                fmt::ptr(this), submissionNumber, slot->nodes.size()));
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

        // Our staging buffer can be read only after this slot's cycle completes.
        // The completion waiter was enqueued before SubmitInternal().
        for (auto &readback : pacmanSceneReadbacks)
            waiterThread.Queue(nullptr, std::move(readback));
        pacmanSceneReadbacks.clear();

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
        ContextLock lock{tag, *request.texture};
        request.texture->PromoteBackingForView(request.format, request.levels);
    }

    void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &request) {
        Submit();
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
