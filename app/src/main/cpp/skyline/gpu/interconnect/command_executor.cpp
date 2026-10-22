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
#include <gpu/diagnostic/sparking_frame_capture.h>
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


    // Phase 2: the first capture proved that 1024x256 auxiliaries starve
    // readback quotas. The recorded arena path is 856x480 HDR -> 856x480
    // RGBA8 -> 856x480 RGBA8 -> 1280x720. Only probe that chain.
    static bool IsSparkingColorImage(const Texture &texture) {
        return texture.dimensions.depth == 1 && texture.levelCount == 1 &&
               texture.layerCount == 1 && texture.sampleCount == vk::SampleCountFlagBits::e1 &&
               texture.surfaceSize > 0 && texture.surfaceSize <= 20ULL * 1024 * 1024 &&
               (texture.format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
               !texture.format->IsCompressed() &&
               (texture.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
               texture.layout != vk::ImageLayout::eUndefined;
    }

    static bool IsSparkingImage(const Texture &texture, std::uint32_t width, std::uint32_t height,
                               vk::Format format) {
        return IsSparkingColorImage(texture) &&
               texture.dimensions.width == width && texture.dimensions.height == height &&
               texture.format->vkFormat == format;
    }

    static bool IsSparkingLdr(const Texture &texture, std::uint32_t width, std::uint32_t height) {
        return IsSparkingImage(texture, width, height, vk::Format::eR8G8B8A8Unorm) ||
               IsSparkingImage(texture, width, height, vk::Format::eB8G8R8A8Unorm);
    }

    std::optional<u32> CommandExecutor::BeginDiagnosticSparkingDraw(span<TextureView *> color) {
        auto &capture{diagnostic::SparkingCapture()};
        if (!capture.active.load(std::memory_order_acquire) || !capture.hdrDrawScan.load(std::memory_order_acquire))
            return std::nullopt;
        for (auto *view : color) {
            if (!view || !view->texture ||
                !IsSparkingImage(*view->texture, 856, 480, vk::Format::eB10G11R11UfloatPack32)) continue;
            auto ordinal{capture.ClaimHdrDraw(reinterpret_cast<uintptr_t>(view->texture.get()),
                                             reinterpret_cast<uintptr_t>(this))};
            if (ordinal && *ordinal == 1) QueueSparkingScanImage(view, "before_first", 0, true);
            return ordinal;
        }
        return std::nullopt;
    }

    void CommandExecutor::EndDiagnosticSparkingDraw(u32 ordinal, span<TextureView *> color) {
        for (auto *view : color) {
            if (!view || !view->texture ||
                !IsSparkingImage(*view->texture, 856, 480, vk::Format::eB10G11R11UfloatPack32)) continue;
            QueueSparkingScanImage(view, "after", ordinal, true);
            break;
        }
    }

    void CommandExecutor::QueueSparkingScanImage(TextureView *view, const std::string &phase,
                                               u32 ordinal, bool hdrStats) {
        auto &capture{diagnostic::SparkingCapture()};
        if (!view || !view->texture || !capture.active.load(std::memory_order_acquire)) return;
        auto texture{view->texture};
        if (texture->dimensions.depth != 1 || texture->levelCount > 12 || texture->layerCount > 6 ||
            texture->sampleCount != vk::SampleCountFlagBits::e1 || !texture->surfaceSize ||
            texture->surfaceSize > 20ULL * 1024 * 1024 ||
            (texture->layout != vk::ImageLayout::eGeneral && texture->layout != vk::ImageLayout::eTransferSrcOptimal) ||
            !(texture->usage & vk::ImageUsageFlagBits::eTransferSrc) ||
            !(texture->format->vkAspect & vk::ImageAspectFlagBits::eColor)) {
            capture.Record(fmt::format("HDR_SCAN_SKIP draw={} phase={} reason=unsupported_image", ordinal, phase));
            return;
        }
        if (hdrStats && texture->surfaceSize != static_cast<size_t>(texture->dimensions.width) * texture->dimensions.height * 4) {
            capture.Record(fmt::format("HDR_SCAN_SKIP draw={} phase={} reason=nonpacked_hdr", ordinal, phase));
            return;
        }
        if (!capture.ReserveScanBytes(texture->surfaceSize)) {
            capture.Record(fmt::format("HDR_SCAN_SKIP draw={} phase={} reason=byte_budget", ordinal, phase));
            return;
        }
        const std::filesystem::path directory{capture.CaptureDirectory()};
        const auto stem{fmt::format("hdr_draw_{:03}_{}", ordinal, phase)};
        const auto bytes{texture->surfaceSize};
        const auto meta{fmt::format("ordinal={} phase={} executor=0x{:X} submission={} rp={} "
            "texture=0x{:X} image={}x{} format={} bytes={} levels={} layers={} "
            "view_mip={} view_levels={} view_layer={} view_layers={} packing=mip_major_all_layers\n",
            ordinal, phase, reinterpret_cast<uintptr_t>(this), submissionNumber, renderPassIndex,
            reinterpret_cast<uintptr_t>(texture.get()), texture->dimensions.width, texture->dimensions.height,
            vk::to_string(texture->format->vkFormat), bytes, texture->levelCount, texture->layerCount,
            view->range.baseMipLevel, view->range.levelCount, view->range.baseArrayLayer, view->range.layerCount)};
        AttachTexture(view); // Reattach if AddSubpass auto-submitted the writer's execution.
        auto staging{gpu.memory.AllocateStagingBuffer(bytes)};
        // Ends the current render pass, then copies in this executor's ordered stream.
        // No submit, CPU readback wait or device-idle is introduced here.
        AddOutsideRpCommand([texture, staging](vk::raii::CommandBuffer &commands,
                                               const std::shared_ptr<FenceCycle> &cycle, GPU &) {
            cycle->AttachObjects(texture, staging);
            texture->CopyIntoStagingBuffer(commands, staging);
            // Order this diagnostic transfer read before any later image writer.
            // CopyIntoStagingBuffer supplies the preceding-write -> read dependency.
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                {}, {}, {}, vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = texture->layout,
                    .newLayout = texture->layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = texture->GetBacking(),
                    .subresourceRange = {texture->format->vkAspect, 0, texture->levelCount, 0, texture->layerCount},
                });
        });
        diagnosticReadbackCallbacks.emplace_back([staging, directory, stem, meta, bytes, ordinal, hdrStats] {
            std::ofstream raw(directory / (stem + ".raw"), std::ios::binary | std::ios::trunc);
            raw.write(reinterpret_cast<const char *>(staging->data()), static_cast<std::streamsize>(bytes));
            if (!raw) { LOGE("MINIRD_SPARKING_DRAW failed to write {}", stem); return; }
            std::ofstream info(directory / (stem + ".txt"), std::ios::trunc);
            info << meta;
            if (hdrStats) {
                const auto stats{diagnostic::SparkingHdrStats::Decode(
                    std::span<const std::uint8_t>{staging->data(), static_cast<size_t>(bytes)})};
                // The selected executor's waiter serializes these callbacks.
                std::ofstream csv(directory / "hdr_draw_stats.csv", std::ios::app);
                csv << ordinal << ',' << stem << ',' << stats.nanPixels << ',' << stats.infPixels << '\n';
            }
            LOGI("MINIRD_SPARKING_DRAW wrote {}", stem);
        });
        capture.Record(fmt::format("HDR_SCAN_COPY ordinal={} phase={} bytes={} metadata={}",
                                   ordinal, phase, bytes, stem));
    }

    void CommandExecutor::TrackDiagnosticPassImages(span<TextureView *> sampled, span<TextureView *> color) {
        auto &capture{diagnostic::SparkingCapture()};
        if (!capture.active.load(std::memory_order_acquire) || capture.hdrDrawScan.load(std::memory_order_acquire))
            return;
        (void)sampled;
        // HDR scene producers do not necessarily sample a prior scene texture.
        // Collect only actual 856x480 B10G11R11 color targets, not every
        // bound auxiliary image. Later postpasses are identified by shader.
        for (auto *view : color) {
            if (!view || !view->texture ||
                !IsSparkingImage(*view->texture, 856, 480, vk::Format::eB10G11R11UfloatPack32))
                continue;
            auto texture{view->texture};
            const auto it{std::find_if(diagnosticPassImages.begin(), diagnosticPassImages.end(),
                [&](const DiagnosticPassImage &entry) { return entry.texture.get() == texture.get(); })};
            if (it == diagnosticPassImages.end() && diagnosticPassImages.size() < 16)
                diagnosticPassImages.push_back({std::move(texture), true, true,
                    diagnostic::SparkingFocusRole::SceneHdr, 0});
        }
    }

    void CommandExecutor::MarkDiagnosticSparkingOutput(span<TextureView *> sampled, span<TextureView *> color,
                                                       std::uint64_t fragmentShaderHash) {
        auto &capture{diagnostic::SparkingCapture()};
        if (!capture.active.load(std::memory_order_acquire) || capture.hdrDrawScan.load(std::memory_order_acquire))
            return;
        // Observed in capture_001. Exact hashes keep the next readback cheap.
        constexpr std::uint64_t ToneMap{0x3332897FCDBD1264ULL};
        constexpr std::uint64_t PostLdr{0x96D28FD4260951CBULL};
        constexpr std::uint64_t Upscale{0x2CFA19C0057B1932ULL};
        if (fragmentShaderHash != ToneMap && fragmentShaderHash != PostLdr &&
            fragmentShaderHash != Upscale)
            return;

        const auto append{[&](span<TextureView *> views, diagnostic::SparkingFocusRole role,
                              std::uint32_t width, std::uint32_t height, bool hdr, bool target) {
            for (auto *view : views) {
                if (!view || !view->texture)
                    continue;
                const auto &texture{view->texture};
                const bool eligible{hdr
                    ? IsSparkingImage(*texture, width, height, vk::Format::eB10G11R11UfloatPack32)
                    : IsSparkingLdr(*texture, width, height)};
                if (!eligible)
                    continue;
                // De-duplicate by role + image identity, NOT by renderpass
                // alone: the same source can also be a target in another pass.
                const auto it{std::find_if(diagnosticPassImages.begin(), diagnosticPassImages.end(),
                    [&](const DiagnosticPassImage &entry) {
                        return entry.texture.get() == texture.get() && entry.role == role;
                    })};
                if (it == diagnosticPassImages.end() && diagnosticPassImages.size() < 16)
                    diagnosticPassImages.push_back({texture, target, true, role, fragmentShaderHash});
                capture.Record(fmt::format(
                    "FOCUS_BINDING shader={:016X} role={} executor=0x{:X} submission={} rp={} "
                    "texture=0x{:X} {}x{} format={} kind={}",
                    fragmentShaderHash, diagnostic::SparkingFocusName(role),
                    reinterpret_cast<uintptr_t>(this), submissionNumber, renderPassIndex,
                    reinterpret_cast<uintptr_t>(texture.get()), width, height,
                    vk::to_string(texture->format->vkFormat), target ? "target" : "sampled"));
                break;
            }
        }};

        if (fragmentShaderHash == ToneMap) {
            append(sampled, diagnostic::SparkingFocusRole::ToneHdrInput, 856, 480, true, false);
            append(color, diagnostic::SparkingFocusRole::ToneLdrOutput, 856, 480, false, true);
        } else if (fragmentShaderHash == PostLdr) {
            append(sampled, diagnostic::SparkingFocusRole::PostLdrInput, 856, 480, false, false);
            append(color, diagnostic::SparkingFocusRole::PostLdrOutput, 856, 480, false, true);
        } else {
            append(sampled, diagnostic::SparkingFocusRole::UpscaleLdrInput, 856, 480, false, false);
            append(color, diagnostic::SparkingFocusRole::UpscaleLdrOutput, 1280, 720, false, true);
        }
    }

    void CommandExecutor::QueueDiagnosticPassReadbacks() {
        if (diagnosticPassImages.empty())
            return;
        auto images{std::move(diagnosticPassImages)};
        diagnosticPassImages.clear();
        auto &capture{diagnostic::SparkingCapture()};
        if (!capture.active.load(std::memory_order_acquire) || capture.hdrDrawScan.load(std::memory_order_acquire))
            return;

        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        if (interval >= diagnostic::SparkingFrameCapture::FrameCount)
            return;
        const std::filesystem::path directory{capture.CaptureDirectory()};
        if (directory.empty())
            return;

        for (const auto &entry : images) {
            const auto &texture{entry.texture};
            if (!entry.priority || !IsSparkingColorImage(*texture))
                continue;

            const auto snapshotId{capture.ReservePassSnapshot(interval, texture->surfaceSize, entry.role)};
            if (!snapshotId)
                continue;
            const auto index{*snapshotId};
            const auto width{texture->dimensions.width};
            const auto height{texture->dimensions.height};
            const auto format{vk::to_string(texture->format->vkFormat)};
            const auto identity{reinterpret_cast<uintptr_t>(texture.get())};
            const auto executorIdentity{reinterpret_cast<uintptr_t>(this)};
            uintptr_t guestMap{};
            size_t guestBytes{};
            if (texture->guest) {
                if (!texture->guest->mappings.empty())
                    guestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                for (const auto &mapping : texture->guest->mappings)
                    guestBytes += mapping.size();
            }
            const auto fileName{fmt::format("pass_{:02}_rp{}_{}x{}_{}.raw",
                index, renderPassIndex, width, height, format)};
            const auto rawPath{directory / fileName};
            const auto metaPath{directory / fmt::format("pass_{:02}.txt", index)};
            const auto meta{fmt::format(
                "capture_index={} approx_present={} render_pass={} submission={} "
                "kind={} focus={} shader={:016X} executor=0x{:X} texture=0x{:X} dimensions={}x{} "
                "format={} layout={} bytes={} guest_map=0x{:X} guest_bytes={} "
                "note=recorded_after_render_pass_end_not_exact_frame_correlation\n",
                index, interval, renderPassIndex, submissionNumber,
                entry.renderTarget ? "render_target" : "sampled_input",
                diagnostic::SparkingFocusName(entry.role), entry.shaderHash, executorIdentity, identity,
                width, height, format, vk::to_string(texture->layout),
                texture->surfaceSize, guestMap, guestBytes)};
            auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
            slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(),
                [texture, staging](vk::raii::CommandBuffer &commandBuffer,
                                   const std::shared_ptr<FenceCycle> &cycle, GPU &) {
                    cycle->AttachObjects(texture, staging);
                    texture->CopyIntoStagingBuffer(commandBuffer, staging);
                });
            diagnosticReadbackCallbacks.emplace_back(
                [staging, rawPath, metaPath, meta, index] {
                    std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
                    if (!raw) {
                        LOGE("MINIRD_SPARKING_PHASE2 failed to open snapshot {}", index);
                        return;
                    }
                    raw.write(reinterpret_cast<const char *>(staging->data()),
                              static_cast<std::streamsize>(staging->size()));
                    raw.close();
                    std::ofstream info(metaPath, std::ios::trunc);
                    if (info)
                        info << meta;
                    LOGI("MINIRD_SPARKING_PHASE2 wrote snapshot {}: {}", index, rawPath.string());
                });
            capture.Record(fmt::format(
                "PHASE2_SNAPSHOT index={} rp={} submission={} executor=0x{:X} interval={} kind={} focus={} shader={:016X} texture=0x{:X} {}x{} format={}",
                index, renderPassIndex, submissionNumber, executorIdentity, interval,
                entry.renderTarget ? "rt" : "sampled", diagnostic::SparkingFocusName(entry.role),
                entry.shaderHash, identity, width, height, format));
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
