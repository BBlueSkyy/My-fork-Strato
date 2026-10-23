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
#include <gpu/diagnostic/ue4_frame_capture.h>
#include <gpu/diagnostic/ue4_upload_provenance.h>
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


    // Target full-resolution UI composition layers, not startup atlases or depth.
    // Like the earlier mini-RenderDoc probes, all image copies remain on the
    // existing command stream and only execute at a render-pass boundary.
    static bool IsUe4ColorImage(const Texture &texture) {
        return texture.dimensions.width >= 256 && texture.dimensions.width <= 2560 &&
               texture.dimensions.height >= 144 && texture.dimensions.height <= 1440 &&
               texture.dimensions.depth == 1 && texture.levelCount == 1 &&
               texture.layerCount == 1 &&
               texture.sampleCount == vk::SampleCountFlagBits::e1 &&
               texture.surfaceSize && texture.surfaceSize <= 20ULL * 1024 * 1024 &&
               (texture.format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
               !texture.format->IsCompressed() &&
               (texture.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
               texture.layout != vk::ImageLayout::eUndefined;
    }

    void CommandExecutor::TrackDiagnosticPassImages(span<TextureView *> sampled, span<TextureView *> color) {
        auto &capture{diagnostic::Ue4Capture()};
        if (!capture.active.load(std::memory_order_acquire))
            return;

        // Follow an already identified output into subsequent postprocessing,
        // including a different command executor. Keep the watch by guest backing
        // because Texture* may change when a view/alias is recreated.
        bool followsUi{};
        for (auto *view : sampled) {
            if (!view || !view->texture || !view->texture->guest)
                continue;
            for (const auto &mapping : view->texture->guest->mappings) {
                if (capture.IsWatchedMapping(reinterpret_cast<uintptr_t>(mapping.data()), mapping.size())) {
                    followsUi = true;
                    break;
                }
            }
            if (followsUi)
                break;
        }
        // Color targets before sampled inputs. Only a candidate output or one
        // of its downstream consumers receives the scarce image readback budget.
        const auto track{[&](span<TextureView *> views, bool rt) {
            for (auto *view : views) {
                if (!view || !view->texture)
                    continue;
                std::shared_ptr<Texture> texture{view->texture};
                if (!IsUe4ColorImage(*texture))
                    continue;
                auto it{std::find_if(diagnosticPassImages.begin(), diagnosticPassImages.end(),
                    [&](const DiagnosticPassImage &candidate) {
                        return candidate.texture.get() == texture.get();
                    })};
                if (it != diagnosticPassImages.end()) {
                    it->renderTarget |= rt;
                    it->priority |= (rt && followsUi);
                } else if (diagnosticPassImages.size() < 24) {
                    diagnosticPassImages.push_back({std::move(texture), rt, rt && followsUi});
                }
            }
        }};
        track(color, true);
        track(sampled, false);
        if (followsUi) {
            for (auto *view : color) {
                if (!view || !view->texture || !IsUe4ColorImage(*view->texture) || !view->texture->guest)
                    continue;
                for (const auto &mapping : view->texture->guest->mappings)
                    capture.WatchMapping(reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
            }
        }
    }

    void CommandExecutor::MarkDiagnosticUe4Output(span<TextureView *> sampled, span<TextureView *> color, bool hasSampledImage) {
        auto &capture{diagnostic::Ue4Capture()};
        if (!hasSampledImage || !capture.active.load(std::memory_order_acquire))
            return;
        // Scene-color input to a render target is a useful HDR/postprocessing
        // transition even when there is no blending or compressed sprite.
        bool samplesColor{};
        for (auto *view : sampled) {
            if (view && view->texture && IsUe4ColorImage(*view->texture)) {
                samplesColor = true;
                break;
            }
        }
        if (!samplesColor)
            return;
        bool hasTarget{};
        for (auto *view : color)
            if (view && view->texture && IsUe4ColorImage(*view->texture))
                hasTarget = true;
        if (!hasTarget)
            return;

        const auto mark{[&](span<TextureView *> views, bool target) {
            for (auto *view : views) {
                if (!view || !view->texture || !IsUe4ColorImage(*view->texture))
                    continue;
                const auto &texture{view->texture};
                const auto it{std::find_if(diagnosticPassImages.begin(), diagnosticPassImages.end(),
                    [&](const DiagnosticPassImage &entry) { return entry.texture.get() == texture.get(); })};
                if (it != diagnosticPassImages.end()) {
                    it->priority = true;
                    it->renderTarget |= target;
                } else if (diagnosticPassImages.size() < 24) {
                    diagnosticPassImages.push_back({texture, target, true});
                }
                if (target && texture->guest) {
                    for (const auto &mapping : texture->guest->mappings)
                        capture.WatchMapping(reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
                }
                capture.Record(fmt::format(
                    "COLOR_PASS_CANDIDATE executor=0x{:X} submission={} rp={} role={} texture=0x{:X} dimensions={}x{} format={}",
                    reinterpret_cast<uintptr_t>(this), submissionNumber, renderPassIndex,
                    target ? "output" : "input", reinterpret_cast<uintptr_t>(texture.get()),
                    texture->dimensions.width, texture->dimensions.height,
                    vk::to_string(texture->format->vkFormat)));
            }
        }};
        mark(sampled, false);
        mark(color, true);
    }

    // GPU-ordered snapshot, not a CPU map. Force a pass boundary so the copy
    // occurs exactly before or after the selected draw's Vulkan commands.
    // Unlike the generic pass tracker, this also accepts tiny 1x1 LUT inputs.
    void CommandExecutor::QueueUe4FocusedImage(TextureView *view, std::string phase, u32 ordinal) {
        auto &capture{diagnostic::Ue4Capture()};
        if (!capture.active.load(std::memory_order_acquire) || !view || !view->texture)
            return;
        const auto texture{std::shared_ptr<Texture>(view->texture)};
        const bool hdrScanPhase{phase.starts_with("hdr_scan_")};
        // The bounded first-HDR capture also needs the real sampled inputs,
        // not just before/after snapshots of the HDR render target.
        const bool firstHdrSource{phase.starts_with("material_input_1_")};
        if (capture.HdrFirstBadMode() && !hdrScanPhase && !firstHdrSource)
            return;
        if (hdrScanPhase &&
            (texture->format->vkFormat != vk::Format::eB10G11R11UfloatPack32 ||
             texture->levelCount != 1 || texture->layerCount != 1 ||
             texture->surfaceSize != static_cast<std::size_t>(texture->dimensions.width) *
                                         texture->dimensions.height * 4)) {
            capture.Record(fmt::format("HDR_SCAN_UNSUPPORTED phase={} format={}",
                                       phase, vk::to_string(texture->format->vkFormat)));
            return;
        }
        if (diagnostic::Ue4Uploads().enabled.load(std::memory_order_relaxed) &&
            (!phase.starts_with("material_input_") || texture->layerCount != 6 ||
             texture->format->vkFormat != vk::Format::eR16G16B16A16Sfloat))
            return; // Provenance mode reuses just the cubemap readback, not the completed HDR/material scan.
        if (texture->layout == vk::ImageLayout::eUndefined ||
            texture->dimensions.depth != 1 || texture->levelCount > 12 ||
            texture->layerCount > 6 || texture->sampleCount != vk::SampleCountFlagBits::e1 ||
            !texture->surfaceSize || texture->surfaceSize > 20ULL * 1024 * 1024 ||
            !(texture->usage & vk::ImageUsageFlagBits::eTransferSrc) ||
            !(texture->format->vkAspect & vk::ImageAspectFlagBits::eColor)) {
            capture.Record(fmt::format("FOCUS_SKIP phase={} draw={} format={} usage={} layout={}",
                phase, ordinal, vk::to_string(texture->format->vkFormat),
                vk::to_string(texture->usage), vk::to_string(texture->layout)));
            return;
        }
        const auto index{capture.ReserveFocusedImage(texture->surfaceSize)};
        if (!index) {
            capture.Record(fmt::format("FOCUS_BUDGET_EXHAUSTED phase={} draw={}", phase, ordinal));
            return;
        }
        const auto directory{std::filesystem::path(capture.CaptureDirectory())};
        const auto id{*index};
        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        const auto filename{fmt::format("focus_{:02}_{}_{}x{}_{}.raw", id, phase,
            texture->dimensions.width, texture->dimensions.height,
            vk::to_string(texture->format->vkFormat))};
        const auto rawPath{directory / filename};
        const auto metaPath{directory / fmt::format("focus_{:02}.txt", id)};
        const auto meta{fmt::format(
            "focus_id={} phase={} hdr_draw_ordinal={} present_index_approx={} executor=0x{:X} "
            "submission={} render_pass={} texture=0x{:X} view_mip={} view_layer={} "
            "image={}x{} format={} layout={} bytes={} mip_levels={} layers={} "
            "packing=mip_major_all_layers vulkan_copy_format=backing\n",
            id, phase, ordinal, interval, reinterpret_cast<uintptr_t>(this),
            submissionNumber, renderPassIndex, reinterpret_cast<uintptr_t>(texture.get()),
            view->range.baseMipLevel, view->range.baseArrayLayer,
            texture->dimensions.width, texture->dimensions.height,
            vk::to_string(texture->format->vkFormat), vk::to_string(texture->layout),
            texture->surfaceSize, texture->levelCount, texture->layerCount)};
        auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
        const bool provenance{phase.starts_with("material_input_") && texture->guest &&
            texture->layerCount == 6 && texture->format->vkFormat == vk::Format::eR16G16B16A16Sfloat};
        // Filled during submission preparation, AFTER attached textures have performed
        // their pending CPU uploads. Retain immutable upload bytes for the writer.
        using ProvenanceResult = std::pair<std::shared_ptr<diagnostic::UploadProof>, std::string>;
        auto provenanceResult{provenance ? std::make_shared<ProvenanceResult>() : nullptr};
        const auto proofPath{directory / fmt::format("focus_{:02}_provenance", id)};
        // AddOutsideRpCommand closes the previous render pass first, then
        // enqueues the copy in the SAME executor's ordered Vulkan stream.
        AddOutsideRpCommand([texture, staging](vk::raii::CommandBuffer &commands,
                                               const std::shared_ptr<FenceCycle> &cycle, GPU &) {
            cycle->AttachObjects(texture, staging);
            texture->CopyIntoStagingBuffer(commands, staging);
        });
        if (provenanceResult) {
            diagnosticUploadCallbacks.emplace_back([texture, provenanceResult] {
                provenanceResult->first = texture->diagnosticUploadProof;
                std::scoped_lock stateLock{texture->stateMutex};
                provenanceResult->second = fmt::format(
                    "readback_guest_format={} readback_host_format={} current_write_serial={} "
                    "current_backing_generation={} ever_gpu_writer={} dirty_state={} memory_freed={} "
                    "alias={} alias_stale={} alias_read_only={} alias_retired={} "
                    "provenance_enabled={} proof_available={} comparison=last_actual_upload_vs_draw_readback "
                    "proof_reason={} sync_attempts={} copy_buffer_to_image_count={} "
                    "proof_discard_count={} budget_used_on_reject={} budget_used_now={} budget_limit={} "
                    "guest_tile_mode={} guest_mirror_bytes={} guest_layer_stride={} "
                    "linear_bytes={} staging_bytes={}\n",
                    vk::to_string(texture->guest->format->vkFormat), vk::to_string(texture->format->vkFormat),
                    texture->gpuWriteSerial, texture->backingGeneration, texture->everUsedAsGpuWrite,
                    static_cast<u32>(texture->dirtyState), texture->memoryFreed, texture->HasAliases(),
                    texture->aliasStale.load(), texture->aliasReadOnly.load(), texture->aliasRetired.load(),
                    diagnostic::Ue4Uploads().enabled.load(), static_cast<bool>(provenanceResult->first),
                    diagnostic::UploadRetentionStatusName(
                        texture->diagnosticUploadStatus.load(std::memory_order_relaxed)),
                    texture->diagnosticUploadSyncAttempts.load(std::memory_order_relaxed),
                    texture->diagnosticUploadCopyCount.load(std::memory_order_relaxed),
                    texture->diagnosticUploadProofDiscards.load(std::memory_order_relaxed),
                    texture->diagnosticUploadBudgetUsedOnReject.load(std::memory_order_relaxed),
                    diagnostic::Ue4Uploads().UsedBytes(), diagnostic::Ue4UploadProvenance::MaxBytes,
                    static_cast<u32>(texture->guest->tileConfig.mode), texture->mirror.size(),
                    texture->guest->GetLayerStride(), texture->deswizzledSurfaceSize,
                    texture->surfaceSize);
            });
        }
        diagnosticReadbackCallbacks.emplace_back(
            [staging, rawPath, metaPath, meta, id, provenanceResult, proofPath,
             hdrScanPhase, ordinal, width = texture->dimensions.width,
             height = texture->dimensions.height] {
                if (hdrScanPhase)
                    diagnostic::Ue4Capture().RecordHdrScan(ordinal, width, height,
                                                            staging->data(), staging->size());
                std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
                if (!raw) {
                    LOGE("MINIRD_UE4_FOCUS failed to write snapshot {}", id);
                    return;
                }
                raw.write(reinterpret_cast<const char *>(staging->data()),
                          static_cast<std::streamsize>(staging->size()));
                raw.close();
                std::ofstream info(metaPath, std::ios::trunc);
                if (info)
                    info << meta;
                if (provenanceResult) {
                    const auto &[proof, current]{*provenanceResult};
                    if (proof) {
                        if (!proof->Write(proofPath, {staging->data(), staging->size()}, current))
                            LOGE("MINIRD_UE4_PROVENANCE write failed: {}", proofPath.string());
                    } else {
                        std::ofstream missing(proofPath.string() + ".txt");
                        missing << current << "No upload proof retained; use proof_reason, sync_attempts, "
                               << "copy_buffer_to_image_count and budget counters above.\n";
                    }
                }
                LOGI("MINIRD_UE4_FOCUS wrote snapshot {}: {}", id, rawPath.string());
            });
        capture.Record(fmt::format("FOCUS_QUEUED id={} phase={} draw={} executor=0x{:X} submit={} image=0x{:X}",
            id, phase, ordinal, reinterpret_cast<uintptr_t>(this),
            submissionNumber, reinterpret_cast<uintptr_t>(texture.get())));
    }

    void CommandExecutor::QueueDiagnosticPassReadbacks() {
        // Phase 3's bounded draw-level probes supersede the original
        // render-pass tracker: those snapshots consumed the entire budget
        // before the HDR->BGRA8 transition.
        diagnosticPassImages.clear();
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

        for (auto &prepare : diagnosticUploadCallbacks) prepare();
        diagnosticUploadCallbacks.clear();
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
