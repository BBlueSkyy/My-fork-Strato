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
#include <gpu/diagnostic/naruto_ui_capture.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
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


    // Ender Lilies probe: full-sized and intermediate 2D color outputs.
    // Geometry/format based, not keyed to a title, shader or driver.
    // Render-pass splits and readbacks remain confined to an armed capture.
    static bool IsDiagnosticSceneTarget(const Texture &texture) {
        return texture.dimensions.width >= 640 && texture.dimensions.width <= 2048 &&
               texture.dimensions.height >= 360 && texture.dimensions.height <= 1440 &&
               texture.dimensions.depth == 1 && texture.levelCount == 1 &&
               texture.layerCount == 1 &&
               texture.sampleCount == vk::SampleCountFlagBits::e1 &&
               texture.surfaceSize && texture.surfaceSize <= 12ULL * 1024 * 1024 &&
               (texture.format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
               !texture.format->IsCompressed() &&
               (texture.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
               texture.layout != vk::ImageLayout::eUndefined;
    }

    static bool IsDiagnosticUiOutput(diagnostic::UiFrameCapture &capture, const Texture &texture) {
        // A Connections UI nameplate may be rendered directly into a full-sized
        // RT without a previously observed Fermi2D flip. Treat every eligible
        // color target as a candidate while the explicit capture is armed.
        // Fermi flip identity remains available in the GPU event history.
        (void)capture;
        return IsDiagnosticSceneTarget(texture);
    }

    static bool IsDiagnosticSceneSource(const Texture &texture) {
        return texture.dimensions.width > 0 && texture.dimensions.width <= 4096 &&
               texture.dimensions.height > 0 && texture.dimensions.height <= 4096 &&
               texture.dimensions.depth == 1 && texture.layerCount == 1 &&
               texture.sampleCount == vk::SampleCountFlagBits::e1 &&
               texture.surfaceSize > 0 && texture.surfaceSize <= 16ULL * 1024 * 1024 &&
               (texture.format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
               (texture.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
               texture.layout != vk::ImageLayout::eUndefined;
    }

    // Readback eligibility is separate from draw selection. RGBA text strips
    // can scale in width from 1184 to 1664 (and vary in height). Select using
    // generic wide-and-shallow geometry instead of a fixed resolution range.
    static bool IsDiagnosticTextStrip(const Texture &texture) {
        return diagnostic::IsWideTextStripGeometry(texture.dimensions.width,
                                                   texture.dimensions.height) &&
               texture.format->vkFormat == vk::Format::eR8G8B8A8Unorm &&
               IsDiagnosticSceneSource(texture);
    }

    static bool IsDiagnosticGlyphAtlas(const Texture &texture) {
        return texture.dimensions.width == 1024 && texture.dimensions.height == 1024 &&
               texture.format->vkFormat == vk::Format::eR8Unorm &&
               IsDiagnosticSceneSource(texture);
    }

    void CommandExecutor::TrackDiagnosticPassImages(span<TextureView *> sampled, span<TextureView *> color) {
        // Stop queuing background RT snapshots at unrelated pass boundaries.
        // Only the explicitly selected post-draw checkpoints arm readbacks.
        (void)sampled;
        (void)color;
    }

    // Diagnostic-only selection by observed image geometry/format. No title IDs,
    // shader hashes, permanent render state or driver-specific behavior.
    struct FocusedFullDraw {
        TextureView *target{};
        TextureView *source{};
        diagnostic::UiFrameCapture::TextFocus kind{};
    };

    static std::optional<FocusedFullDraw> FindFocusedFullDraw(span<TextureView *> sampled,
                                                             span<TextureView *> color,
                                                             bool blendingEnabled) {
        // Two independent first-draw probes: blended and opaque. Both can hide
        // an incorrect scene, so never require text strips or a BC3 logo.
        TextureView *target{};
        for (auto *view : color)
            if (view && view->texture && IsDiagnosticSceneTarget(*view->texture)) {
                target = view;
                break;
            }
        if (!target)
            return std::nullopt;

        for (auto *view : sampled) {
            if (!view || !view->texture || view->texture.get() == target->texture.get())
                continue;
            if (IsDiagnosticSceneSource(*view->texture))
                return FocusedFullDraw{target, view,
                    blendingEnabled ? diagnostic::UiFrameCapture::TextFocus::FullConsumer
                                    : diagnostic::UiFrameCapture::TextFocus::FullSprite};
        }
        return std::nullopt;
    }

    bool CommandExecutor::ProbeDiagnosticUiPreDraw(span<TextureView *> sampled, span<TextureView *> color, bool blendingEnabled) {
        auto &capture{diagnostic::NarutoUiCapture()};
        diagnosticPendingPreDrawId = 0;
        if (!capture.active.load(std::memory_order_acquire))
            return false;
        const auto match{FindFocusedFullDraw(sampled, color, blendingEnabled)};
        if (!match)
            return false;

        // Snapshot a real descriptor-resolved source, target before the draw,
        // and its output afterward. The two quotas cover opaque and blended
        // full-scene draws in each of four approximate presentation intervals.
        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        const bool sprite{match->kind == diagnostic::UiFrameCapture::TextFocus::FullSprite};
        if (!(sprite ? capture.ClaimPreSpriteDraw(interval) : capture.ClaimPreUiDraw(interval)))
            return false;

        const auto drawId{capture.NextFocusedDrawId()};
        diagnosticPendingPreDrawId = drawId;
        const auto sourceCycle{match->source->texture->cycle.get()};
        const auto targetCycle{match->target->texture->cycle.get()};
        // This split orders the baseline copy strictly ahead of the draw in
        // the same GPU command stream. It is never enabled in normal rendering.
        BreakRenderPass();
        diagnosticPassImages.push_back({match->target->texture, true, true, false, true,
                                        drawId, "scene_target_before"});
        // Capture the descriptor-resolved source BEFORE it is sampled. Copying
        // it after the draw could falsely blame a producer when the output
        // aliases the source backing or a subsequent writer modifies it.
        diagnosticPassImages.push_back({match->source->texture, false, true, false, true,
                                        drawId, "scene_source_before"});
        capture.Record(fmt::format(
            "SCENE_CONSUMER_PRE draw_id={} kind={} interval={} executor=0x{:X} "
            "submission={} rp={} source=0x{:X} source_cycle=0x{:X} "
            "target=0x{:X} target_cycle=0x{:X}",
            drawId, sprite ? "opaque" : "blended", interval,
            reinterpret_cast<uintptr_t>(this), submissionNumber, renderPassIndex,
            reinterpret_cast<uintptr_t>(match->source->texture.get()),
            reinterpret_cast<uintptr_t>(sourceCycle),
            reinterpret_cast<uintptr_t>(match->target->texture.get()),
            reinterpret_cast<uintptr_t>(targetCycle)));
        QueueDiagnosticPassReadbacks();
        return true;
    }

    void CommandExecutor::MarkDiagnosticUiOutput(span<TextureView *> sampled, span<TextureView *> color,
                                                  bool blendingEnabled, std::string_view shaderHashes,
                                                  u32 drawArgument, bool indexed) {
        auto &capture{diagnostic::NarutoUiCapture()};
        if (!capture.active.load(std::memory_order_acquire))
            return;
        const auto match{FindFocusedFullDraw(sampled, color, blendingEnabled)};
        if (!match) {
            diagnosticPendingPreDrawId = 0;
            return;
        }
        // Mark the *produced* full-resolution target, not the background
        // Fermi2D input. The following reverse blit can then snapshot
        // precisely the texture that received these compositor draws.
        capture.ObserveUiCompositorTarget(reinterpret_cast<uintptr_t>(match->target->texture.get()));
        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        const bool sprite{match->kind == diagnostic::UiFrameCapture::TextFocus::FullSprite};
        const auto drawId{diagnosticPendingPreDrawId ? diagnosticPendingPreDrawId : capture.NextFocusedDrawId()};
        diagnosticPendingPreDrawId = 0;

        const auto guestAddress{[](const TextureView *view) -> uintptr_t {
            if (!view || !view->texture || !view->texture->guest ||
                view->texture->guest->mappings.empty())
                return 0;
            return reinterpret_cast<uintptr_t>(view->texture->guest->mappings.front().data());
        }};
        // Log ALL qualifying consumers, even those not selected for readback.
        // Unlike a sampled generic DRAW event, these retain actual resolved
        // descriptors and are explicitly linked to a CPU draw ID.
        std::string bindings;
        for (std::size_t i{}; i < sampled.size() && i < 16; ++i) {
            const auto *view{sampled[i]};
            if (!view || !view->texture)
                continue;
            const auto &texture{*view->texture};
            bindings += fmt::format(
                "[{}]=0x{:X}:{}x{}:{}:guest=0x{:X}:cycle=0x{:X},",
                i, reinterpret_cast<uintptr_t>(&texture),
                texture.dimensions.width, texture.dimensions.height,
                vk::to_string(texture.format->vkFormat), guestAddress(view),
                reinterpret_cast<uintptr_t>(texture.cycle.get()));
        }
        capture.Record(fmt::format(
            "SCENE_CONSUMER_DRAW draw_id={} kind={} indexed={} draw_arg={} "
            "shaders={} executor=0x{:X} submission={} rp={} "
            "target=0x{:X} guest=0x{:X} target_cycle=0x{:X} "
            "source=0x{:X} source_guest=0x{:X} source_cycle=0x{:X} "
            "source_mips={} source_layers={} bindings={}",
            drawId, sprite ? "opaque" : "blended", indexed,
            drawArgument, shaderHashes, reinterpret_cast<uintptr_t>(this),
            submissionNumber, renderPassIndex,
            reinterpret_cast<uintptr_t>(match->target->texture.get()),
            guestAddress(match->target),
            reinterpret_cast<uintptr_t>(match->target->texture->cycle.get()),
            reinterpret_cast<uintptr_t>(match->source->texture.get()),
            guestAddress(match->source),
            reinterpret_cast<uintptr_t>(match->source->texture->cycle.get()),
            match->source->range.levelCount, match->source->range.layerCount,
            bindings));

        // The earlier Naruto-specific fourth-text compositor probe is
        // deliberately disabled for scene diagnostics. Paired draws below
        // provide the needed ordered evidence within the existing quota.

        const auto ordinal{capture.ClaimTextFocus(interval, match->kind)};
        if (!ordinal)
            return;
        // The pre-draw source and destination have already been queued on
        // this command stream. Only the destination needs a post-draw copy.
        diagnosticPassImages.clear();
        diagnosticPassImages.push_back({match->target->texture, true, true, false, false,
                                        drawId, "scene_target_after"});
        capture.Record(fmt::format("SCENE_FOCUS stage={} ordinal={} draw_id={} "
                                   "executor=0x{:X} submission={} rp={}",
                                   sprite ? "opaque_to_full" : "blended_to_full",
                                   *ordinal, drawId, reinterpret_cast<uintptr_t>(this),
                                   submissionNumber, renderPassIndex));
        BreakRenderPass();
    }

    void CommandExecutor::ProbeDiagnosticFermiInput(TextureView *source, TextureView *destination) {
        auto &capture{diagnostic::NarutoUiCapture()};
        if (!capture.active.load(std::memory_order_acquire) || !source || !destination ||
            !source->texture || !destination->texture ||
            !IsDiagnosticSceneTarget(*source->texture) ||
            source->texture.get() == destination->texture.get())
            return;

        const auto src{source->texture};
        const auto dst{destination->texture};
        // The reverse blit ends one logical UI composition cycle. Do this
        // even if the pre-Fermi readback budget was exhausted.
        capture.CompleteUiCompositorCycle(reinterpret_cast<uintptr_t>(src.get()));
        const auto id{capture.ReserveFermiSource(reinterpret_cast<uintptr_t>(src.get()),
                                                 reinterpret_cast<uintptr_t>(dst.get()),
                                                 src->surfaceSize)};
        if (!id)
            return;
        // Same GPU command stream as the imminent blit, with no CPU GPU-fence
        // wait. This captures pixels BEFORE Fermi2D samples the source.
        BreakRenderPass();
        auto staging{gpu.memory.AllocateStagingBuffer(src->surfaceSize)};
        slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(),
            [src, staging](vk::raii::CommandBuffer &commandBuffer,
                           const std::shared_ptr<FenceCycle> &cycle, GPU &) {
                cycle->AttachObjects(src, staging);
                src->CopyIntoStagingBuffer(commandBuffer, staging);
                RecordFullBarrier(commandBuffer);
            });

        const auto directory{std::filesystem::path(capture.CaptureDirectory())};
        const auto rawName{fmt::format("fermi_input_{:02}_{}x{}_{}.raw",
                                      *id, src->dimensions.width, src->dimensions.height,
                                      vk::to_string(src->format->vkFormat))};
        const auto rawPath{directory / rawName};
        const auto metaPath{directory / fmt::format("fermi_input_{:02}.txt", *id)};
        const auto mapAddress{[](const auto &texture) -> uintptr_t {
            return texture->guest && !texture->guest->mappings.empty()
                ? reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data()) : 0;
        }};
        const auto meta{fmt::format(
            "fermi_input_id={} approx_present={} executor=0x{:X} submission={} "
            "source=0x{:X} dest=0x{:X} source_guest=0x{:X} dest_guest=0x{:X} "
            "source_cycle=0x{:X} dest_cycle=0x{:X} dimensions={}x{} format={} "
            "note=GPU_ordered_after_UI_compositor_before_reverse_Fermi_blit_not_exact_present_frame\n",
            *id, capture.presentIndex.load(std::memory_order_acquire),
            reinterpret_cast<uintptr_t>(this), submissionNumber,
            reinterpret_cast<uintptr_t>(src.get()), reinterpret_cast<uintptr_t>(dst.get()),
            mapAddress(src), mapAddress(dst),
            reinterpret_cast<uintptr_t>(src->cycle.get()),
            reinterpret_cast<uintptr_t>(dst->cycle.get()),
            src->dimensions.width, src->dimensions.height, vk::to_string(src->format->vkFormat))};
        diagnosticReadbackCallbacks.emplace_back([staging, rawPath, metaPath, meta, id = *id] {
            std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
            if (!raw) {
                LOGE("MINIRD_CONNECTIONS Fermi input {} could not open output", id);
                return;
            }
            raw.write(reinterpret_cast<const char *>(staging->data()),
                      static_cast<std::streamsize>(staging->size()));
            raw.close();
            std::ofstream info(metaPath, std::ios::trunc);
            if (info)
                info << meta;
            LOGI("MINIRD_CONNECTIONS wrote Fermi input {}: {}", id, rawPath.string());
        });
        capture.Record(fmt::format(
            "FERMI_COMPOSITED_UI_SOURCE id={} executor=0x{:X} submission={} "
            "source=0x{:X} dest=0x{:X} source_guest=0x{:X} dest_guest=0x{:X}",
            *id, reinterpret_cast<uintptr_t>(this), submissionNumber,
            reinterpret_cast<uintptr_t>(src.get()), reinterpret_cast<uintptr_t>(dst.get()),
            mapAddress(src), mapAddress(dst)));
    }

    void CommandExecutor::QueueDiagnosticPassReadbacks() {
        if (diagnosticPassImages.empty())
            return;
        auto images{std::move(diagnosticPassImages)};
        diagnosticPassImages.clear();
        auto &capture{diagnostic::NarutoUiCapture()};
        if (!capture.active.load(std::memory_order_acquire))
            return;

        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        if (interval >= diagnostic::UiFrameCapture::FrameCount)
            return;
        const std::filesystem::path directory{capture.CaptureDirectory()};
        if (directory.empty())
            return;

        for (const auto &entry : images) {
            const auto &texture{entry.texture};
            if (!entry.priority)
                continue;
            const bool atlas{entry.atlas};
            if (atlas ? !IsDiagnosticSceneSource(*texture) :
                      !(IsDiagnosticSceneTarget(*texture) || IsDiagnosticTextStrip(*texture) ||
                        (!entry.renderTarget && IsDiagnosticSceneSource(*texture))))
                continue;

            const auto identity{reinterpret_cast<uintptr_t>(texture.get())};
            const auto snapshotId{atlas ?
                capture.ReserveAtlasSnapshot(interval, identity, texture->surfaceSize) :
                capture.ReservePassSnapshot(interval, texture->surfaceSize)};
            if (!snapshotId)
                continue;
            const auto index{*snapshotId};
            const auto width{texture->dimensions.width};
            const auto height{texture->dimensions.height};
            const auto format{vk::to_string(texture->format->vkFormat)};
            const auto executorIdentity{reinterpret_cast<uintptr_t>(this)};
            uintptr_t guestMap{};
            size_t guestBytes{};
            if (texture->guest) {
                if (!texture->guest->mappings.empty())
                    guestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                for (const auto &mapping : texture->guest->mappings)
                    guestBytes += mapping.size();
            }
            const auto prefix{atlas ? "atlas" : entry.beforeUiDraw ? "pre" : "pass"};
            const auto fileName{fmt::format("{}_{:02}_rp{}_{}x{}_{}.raw",
                prefix, index, renderPassIndex, width, height, format)};
            const auto rawPath{directory / fileName};
            const auto metaPath{directory / fmt::format("{}_{:02}.txt", prefix, index)};
            const auto meta{fmt::format(
                "capture_index={} approx_present={} render_pass={} submission={} "
                "kind={} focus={} executor=0x{:X} texture=0x{:X} dimensions={}x{} "
                "format={} layout={} bytes={} guest_map=0x{:X} guest_bytes={} mips={} layers={} "
                "draw_id={} role={} note=late_draw_boundary_approximate_presentation\n",
                index, interval, renderPassIndex, submissionNumber,
                atlas ? "sampled_atlas" : entry.beforeUiDraw ?
                    (entry.renderTarget ? "pre_ui_render_target" : "sampled_input_before_draw") :
                    entry.renderTarget ? "render_target" : "sampled_input",
                atlas ? "glyph_or_sampled_source" : entry.beforeUiDraw ?
                    "before_first_matching_consumer_draw" : "consumer_output",
                executorIdentity, identity, width, height, format, vk::to_string(texture->layout),
                texture->surfaceSize, guestMap, guestBytes, texture->levelCount, texture->layerCount,
                entry.drawId, entry.role ? entry.role : "legacy")};
            auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
            slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(),
                [texture, staging, beforeUiDraw = entry.beforeUiDraw](vk::raii::CommandBuffer &commandBuffer,
                                   const std::shared_ptr<FenceCycle> &cycle, GPU &) {
                    cycle->AttachObjects(texture, staging);
                    texture->CopyIntoStagingBuffer(commandBuffer, staging);
                    if (beforeUiDraw)
                        RecordFullBarrier(commandBuffer); // Transfer read must complete before the next color write.
                });
            diagnosticReadbackCallbacks.emplace_back(
                [staging, rawPath, metaPath, meta, index] {
                    std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
                    if (!raw) {
                        LOGE("MINIRD_CONNECTIONS_PHASE2 failed to open snapshot {}", index);
                        return;
                    }
                    raw.write(reinterpret_cast<const char *>(staging->data()),
                              static_cast<std::streamsize>(staging->size()));
                    raw.close();
                    std::ofstream info(metaPath, std::ios::trunc);
                    if (info)
                        info << meta;
                    LOGI("MINIRD_CONNECTIONS_PHASE2 wrote snapshot {}: {}", index, rawPath.string());
                });
            capture.Record(fmt::format(
                "PHASE2_SNAPSHOT index={} rp={} submission={} executor=0x{:X} interval={} kind={} texture=0x{:X} {}x{} format={} draw_id={} role={}",
                index, renderPassIndex, submissionNumber, executorIdentity, interval,
                atlas ? "atlas" : entry.beforeUiDraw ? "pre_ui" : entry.renderTarget ? "rt" : "sampled", identity, width, height, format,
                entry.drawId, entry.role ? entry.role : "legacy"));
        }
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
                QueueDiagnosticPassReadbacks();
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

        TrackDiagnosticPassImages(sampledImages, colorAttachments);
        return gotoNext;
    }

    void CommandExecutor::FinishRenderPass() {
        if (renderPass) {
            for (auto &callback : subpassBoundaryCallbacks)
                callback();

            slot->nodes.emplace_back(std::in_place_type_t<node::RenderPassEndNode>());
            slot->nodes.splice(slot->nodes.end(), slot->pendingPostRenderPassNodes);
            QueueDiagnosticPassReadbacks();
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
        // SubmitInternal() closes the final render pass and may enqueue more
        // diagnostic readbacks. Queue file writes AFTER that point so callbacks
        // belong to the correct GPU cycle (already registered above).
        for (auto &cb : diagnosticReadbackCallbacks)
            waiterThread.Queue(nullptr, std::move(cb));
        diagnosticReadbackCallbacks.clear();

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
