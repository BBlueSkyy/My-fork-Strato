// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <range/v3/view.hpp>
#include <adrenotools/driver.h>
#include <common/settings.h>
#include <loader/loader.h>
#include <gpu.h>
#include <os.h>
#include <dlfcn.h>
#include "command_executor.h"
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

    static bool IsMiniRenderDocTarget(const Texture &texture) {
        if (texture.levelCount != 1 || texture.layerCount != 1 ||
            texture.sampleCount != vk::SampleCountFlagBits::e1 ||
            !(texture.format->vkAspect & vk::ImageAspectFlagBits::eColor))
            return false;

        const auto format{texture.format->vkFormat};
        if (format != vk::Format::eR8G8B8A8Unorm &&
            format != vk::Format::eR16G16B16A16Sfloat)
            return false;

        const auto width{texture.dimensions.width};
        const auto height{texture.dimensions.height};
        return (width == 1280 && height == 720) ||
               (width == 640 && height == 720) ||
               (width == 320 && height == 360) ||
               (width == 1920 && height == 1080);
    }

    static bool IsMiniRenderDocHdrTrigger(const Texture &texture) {
        return texture.dimensions.width == 1280 &&
               texture.dimensions.height == 720 &&
               texture.format->vkFormat == vk::Format::eR16G16B16A16Sfloat;
    }

    static bool IsMiniRenderDocFrameStart(const Texture &texture) {
        return texture.dimensions.width == 1280 &&
               texture.dimensions.height == 720 &&
               texture.format->vkFormat == vk::Format::eR8G8B8A8Unorm;
    }

    static bool IsMiniRenderDocFullHdOutput(const Texture &texture) {
        return texture.dimensions.width == 1920 &&
               texture.dimensions.height == 1080 &&
               texture.format->vkFormat == vk::Format::eR8G8B8A8Unorm;
    }

    void CommandExecutor::TrackDiagnosticRenderTargets(span<TextureView *> colorAttachments) {
        if (diagnosticCaptureState == DiagnosticCaptureState::Complete)
            return;

        for (auto *view : colorAttachments) {
            if (!view)
                continue;

            std::shared_ptr<Texture> texture{view->texture};
            if (std::find_if(diagnosticRenderTargets.begin(), diagnosticRenderTargets.end(), [&](const auto &existing) {
                    return existing.get() == texture.get();
                }) == diagnosticRenderTargets.end())
                diagnosticRenderTargets.emplace_back(std::move(texture));
        }
    }

    bool CommandExecutor::CheckDiagnosticCaptureArm() {
        if (diagnosticCaptureArmed)
            return true;

        std::filesystem::path base{state.os->publicAppFilesPath};
        base /= "gpu_capture";
        base /= "nine_sols";

        std::error_code error;
        std::filesystem::create_directories(base, error);
        if (error) {
            LOGE("MINIRD failed to create arm directory '{}': {}", base.string(), error.message());
            return false;
        }

        const auto armPath{base / "ARM_CAPTURE"};
        error.clear();
        if (std::filesystem::exists(armPath, error) && !error) {
            std::filesystem::remove(armPath, error);
            diagnosticCaptureArmed = true;
            diagnosticCaptureState = DiagnosticCaptureState::WaitingForHdr;
            diagnosticSampledInputs.clear();
            diagnosticSampledInputsCaptured = false;
            diagnosticDrawTraceLines.clear();
            diagnosticDrawTraceFlushedCount = 0;
            LOGI("MINIRD arm marker consumed; waiting for the next HDR chain");
            return true;
        }

        const auto readyPath{base / "ARM_CAPTURE.rename_this_file_when_ready"};
        error.clear();
        if (!std::filesystem::exists(readyPath, error) && !error) {
            std::ofstream ready{readyPath, std::ios::out | std::ios::trunc};
            if (ready) {
                ready
                    << "Reach the Nine Sols black scene, switch to a file manager,\n"
                    << "rename this file to exactly ARM_CAPTURE, then return to Strato.\n"
                    << "The capture starts on the next complete render chain and stops automatically.\n";
            }
        }

        return false;
    }

    void CommandExecutor::QueueDiagnosticSampledInputCaptures() {
        if (diagnosticSampledInputsCaptured ||
            diagnosticCaptureState != DiagnosticCaptureState::Capturing ||
            renderPassIndex != 8 ||
            diagnosticCaptureDirectory.empty() ||
            diagnosticSampledInputs.empty())
            return;

        const std::filesystem::path captureDirectory{diagnosticCaptureDirectory};

        for (size_t inputIndex{}; inputIndex < diagnosticSampledInputs.size(); ++inputIndex) {
            const auto &texture{diagnosticSampledInputs[inputIndex]};
            if (!texture ||
                texture->layout == vk::ImageLayout::eUndefined ||
                !texture->surfaceSize ||
                texture->sampleCount != vk::SampleCountFlagBits::e1)
                continue;

            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
            const auto width{texture->dimensions.width};
            const auto height{texture->dimensions.height};
            const auto depth{texture->dimensions.depth};
            const auto formatName{vk::to_string(texture->format->vkFormat)};
            const auto fileName{fmt::format("rp8_input_{:02}_{}x{}x{}_{}.raw",
                                            inputIndex, width, height, depth, formatName)};
            const auto rawPath{captureDirectory / fileName};
            const auto metadataPath{captureDirectory / fmt::format("rp8_input_{:02}.txt", inputIndex)};

            uintptr_t guestMap{};
            size_t guestMapSize{};
            u32 tileMode{};
            u32 blockHeight{};
            u32 blockDepth{};
            if (texture->guest) {
                if (!texture->guest->mappings.empty())
                    guestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                for (const auto &mapping : texture->guest->mappings)
                    guestMapSize += mapping.size();
                tileMode = static_cast<u32>(texture->guest->tileConfig.mode);
                if (texture->guest->tileConfig.mode == texture::TileMode::Block) {
                    blockHeight = texture->guest->tileConfig.blockHeight;
                    blockDepth = texture->guest->tileConfig.blockDepth;
                }
            }

            const auto textureAddress{reinterpret_cast<uintptr_t>(texture.get())};
            const auto metadata{fmt::format(
                "input_index={}\nfile={}\nrender_pass={}\nsubmission={}\ntexture=0x{:X}\n"
                "width={}\nheight={}\ndepth={}\nformat={}\nlayout={}\nsize={}\n"
                "levels={}\nlayers={}\nguest_map=0x{:X}\nguest_map_size={}\ntile={}\nbh={}\nbd={}\n",
                inputIndex, fileName, renderPassIndex, submissionNumber, textureAddress,
                width, height, depth, formatName, vk::to_string(texture->layout), texture->surfaceSize,
                texture->levelCount, texture->layerCount,
                guestMap, guestMapSize, tileMode, blockHeight, blockDepth)};

            slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(),
                [texture, stagingBuffer](vk::raii::CommandBuffer &commandBuffer,
                                         const std::shared_ptr<FenceCycle> &cycle,
                                         GPU &) {
                    cycle->AttachObjects(texture, stagingBuffer);
                    texture->CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
                });

            pendingDiagnosticCaptureCallbacks.emplace_back(
                [stagingBuffer, rawPath, metadataPath, metadata, inputIndex] {
                    std::ofstream raw{rawPath, std::ios::out | std::ios::binary | std::ios::trunc};
                    if (!raw) {
                        LOGE("MINIRD failed to open rp8 sampled input {}", inputIndex);
                        return;
                    }
                    raw.write(reinterpret_cast<const char *>(stagingBuffer->data()),
                              static_cast<std::streamsize>(stagingBuffer->size()));
                    raw.close();

                    std::ofstream meta{metadataPath, std::ios::out | std::ios::trunc};
                    if (meta)
                        meta << metadata;

                    LOGI("MINIRD wrote rp8 sampled input {}: {}", inputIndex, rawPath.string());
                });
        }

        diagnosticSampledInputsCaptured = true;
        diagnosticSampledInputs.clear();
    }

    void CommandExecutor::FlushDiagnosticDrawTrace() {
        if (diagnosticCaptureDirectory.empty() ||
            diagnosticDrawTraceFlushedCount >= diagnosticDrawTraceLines.size())
            return;

        const std::filesystem::path tracePath{
            std::filesystem::path(diagnosticCaptureDirectory) / "rp8_draws.txt"
        };

        const bool createHeader{diagnosticDrawTraceFlushedCount == 0};
        std::ofstream trace{tracePath, std::ios::out | std::ios::app};
        if (!trace) {
            LOGE("MINIRD failed to open rp8 draw trace '{}'", tracePath.string());
            return;
        }

        if (createHeader) {
            trace
                << "Strato mini-RenderDoc targeted draw trace\n"
                << "target=render_pass_8\n"
                << "purpose=shader_descriptor_state_and_sampled_view_identity\n"
                << "note=diagnostic_only_no_guest_memory_writes\n\n";
        }

        for (; diagnosticDrawTraceFlushedCount < diagnosticDrawTraceLines.size();
             ++diagnosticDrawTraceFlushedCount) {
            trace << diagnosticDrawTraceLines[diagnosticDrawTraceFlushedCount];
            if (!diagnosticDrawTraceLines[diagnosticDrawTraceFlushedCount].empty() &&
                diagnosticDrawTraceLines[diagnosticDrawTraceFlushedCount].back() != '\n')
                trace << '\n';
            trace << '\n';
        }
    }

    bool CommandExecutor::EnsureDiagnosticCaptureDirectory() {
        if (!diagnosticCaptureDirectory.empty())
            return true;

        std::filesystem::path base{state.os->publicAppFilesPath};
        base /= "gpu_capture";
        base /= "nine_sols";

        std::error_code error;
        std::filesystem::create_directories(base, error);
        if (error) {
            LOGE("MINIRD failed to create capture root '{}': {}", base.string(), error.message());
            diagnosticCaptureState = DiagnosticCaptureState::Complete;
            return false;
        }

        for (u32 index{1}; index <= 999; ++index) {
            auto candidate{base / fmt::format("capture_{:03}", index)};
            error.clear();
            const bool exists{std::filesystem::exists(candidate, error)};
            if (error)
                continue;
            if (exists)
                continue;

            std::filesystem::create_directories(candidate, error);
            if (error)
                continue;

            diagnosticCaptureDirectory = candidate.string();
            std::ofstream manifest{candidate / "manifest.txt", std::ios::out | std::ios::trunc};
            if (!manifest) {
                LOGE("MINIRD failed to create manifest in '{}'", diagnosticCaptureDirectory);
                diagnosticCaptureState = DiagnosticCaptureState::Complete;
                return false;
            }

            manifest
                << "Strato mini-RenderDoc diagnostic capture\n"
                << "scope=Nine Sols render-target chain\n"
                << "raw_layout=linear host image bytes\n"
                << "mips=1\n"
                << "layers=1\n"
                << "arm=ARM_CAPTURE marker consumed\n"
                << "trigger=first 1280x720 RGBA8 pass after observing 1280x720 RGBA16F\n"
                << "stop=second 1920x1080 RGBA8 snapshot or 32 snapshots\n"
                << "guest_memory_modified=false\n"
                << "columns=index,file,render_pass,submission,texture,width,height,format,layout,size,guest_map,guest_map_size,tile,bh,bd\n";
            manifest.close();

            LOGI("MINIRD capture directory: {}", diagnosticCaptureDirectory);
            return true;
        }

        LOGE("MINIRD could not allocate a capture directory under '{}'", base.string());
        diagnosticCaptureState = DiagnosticCaptureState::Complete;
        return false;
    }

    void CommandExecutor::QueueDiagnosticRenderTargetCaptures() {
        if (diagnosticRenderTargets.empty() ||
            diagnosticCaptureState == DiagnosticCaptureState::Complete) {
            diagnosticRenderTargets.clear();
            return;
        }

        if (!CheckDiagnosticCaptureArm()) {
            diagnosticRenderTargets.clear();
            return;
        }

        FlushDiagnosticDrawTrace();

        if (diagnosticCaptureState == DiagnosticCaptureState::WaitingForHdr) {
            if (ranges::any_of(diagnosticRenderTargets, [](const auto &texture) {
                    return IsMiniRenderDocHdrTrigger(*texture);
                })) {
                diagnosticCaptureState = DiagnosticCaptureState::WaitingForFrameStart;
                LOGI("MINIRD armed after observing 1280x720 RGBA16F");
            }
            diagnosticRenderTargets.clear();
            return;
        }

        if (diagnosticCaptureState == DiagnosticCaptureState::WaitingForFrameStart) {
            if (!ranges::any_of(diagnosticRenderTargets, [](const auto &texture) {
                    return IsMiniRenderDocFrameStart(*texture);
                })) {
                diagnosticRenderTargets.clear();
                return;
            }

            if (!EnsureDiagnosticCaptureDirectory()) {
                diagnosticRenderTargets.clear();
                return;
            }

            diagnosticCaptureState = DiagnosticCaptureState::Capturing;
            FlushDiagnosticDrawTrace();
            LOGI("MINIRD starting capture at render pass {}", renderPassIndex);
        }

        bool captureComplete{};
        for (const auto &texture : diagnosticRenderTargets) {
            if (diagnosticCaptureState != DiagnosticCaptureState::Capturing ||
                !IsMiniRenderDocTarget(*texture) ||
                texture->layout == vk::ImageLayout::eUndefined ||
                !texture->surfaceSize)
                continue;

            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
            const size_t captureIndex{diagnosticCaptureIndex++};
            const auto width{texture->dimensions.width};
            const auto height{texture->dimensions.height};
            const auto format{texture->format->vkFormat};
            const auto formatName{vk::to_string(format)};
            const auto layoutName{vk::to_string(texture->layout)};
            const auto suffix{format == vk::Format::eR16G16B16A16Sfloat ? "rgba16f" : "rgba8"};
            const auto fileName{fmt::format("{:03}_rp{}_{}x{}_{}.raw",
                                           captureIndex, renderPassIndex, width, height, suffix)};
            const std::filesystem::path captureDirectory{diagnosticCaptureDirectory};
            const auto rawPath{captureDirectory / fileName};
            const auto metadataPath{captureDirectory / fmt::format("{:03}_rp{}.txt", captureIndex, renderPassIndex)};
            const auto manifestPath{captureDirectory / "manifest.txt"};

            uintptr_t guestMap{};
            size_t guestMapSize{};
            u32 tileMode{};
            u32 blockHeight{};
            u32 blockDepth{};
            if (texture->guest) {
                if (!texture->guest->mappings.empty())
                    guestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                for (const auto &mapping : texture->guest->mappings)
                    guestMapSize += mapping.size();
                tileMode = static_cast<u32>(texture->guest->tileConfig.mode);
                if (texture->guest->tileConfig.mode == texture::TileMode::Block) {
                    blockHeight = texture->guest->tileConfig.blockHeight;
                    blockDepth = texture->guest->tileConfig.blockDepth;
                }
            }

            const auto textureAddress{reinterpret_cast<uintptr_t>(texture.get())};
            const auto metadata{fmt::format(
                "index={}\nfile={}\nrender_pass={}\nsubmission={}\ntexture=0x{:X}\n"
                "width={}\nheight={}\nformat={}\nlayout={}\nsize={}\n"
                "guest_map=0x{:X}\nguest_map_size={}\ntile={}\nbh={}\nbd={}\n",
                captureIndex, fileName, renderPassIndex, submissionNumber, textureAddress,
                width, height, formatName, layoutName, texture->surfaceSize,
                guestMap, guestMapSize, tileMode, blockHeight, blockDepth)};

            const auto manifestLine{fmt::format(
                "{},{},{},{},0x{:X},{},{},{},{},{},0x{:X},{},{},{},{}\n",
                captureIndex, fileName, renderPassIndex, submissionNumber, textureAddress,
                width, height, formatName, layoutName, texture->surfaceSize,
                guestMap, guestMapSize, tileMode, blockHeight, blockDepth)};

            slot->nodes.emplace_back(std::in_place_type_t<node::FunctionNode>(),
                [texture, stagingBuffer](vk::raii::CommandBuffer &commandBuffer,
                                         const std::shared_ptr<FenceCycle> &cycle,
                                         GPU &) {
                    cycle->AttachObjects(texture, stagingBuffer);
                    texture->CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
                });

            pendingDiagnosticCaptureCallbacks.emplace_back(
                [stagingBuffer, rawPath, metadataPath, manifestPath, metadata, manifestLine, captureIndex] {
                    std::ofstream raw{rawPath, std::ios::out | std::ios::binary | std::ios::trunc};
                    if (!raw) {
                        LOGE("MINIRD failed to open raw snapshot {}", captureIndex);
                        return;
                    }
                    raw.write(reinterpret_cast<const char *>(stagingBuffer->data()),
                              static_cast<std::streamsize>(stagingBuffer->size()));
                    raw.close();

                    std::ofstream meta{metadataPath, std::ios::out | std::ios::trunc};
                    if (meta) {
                        meta << metadata;
                        meta.close();
                    }

                    std::ofstream manifest{manifestPath, std::ios::out | std::ios::app};
                    if (manifest) {
                        manifest << manifestLine;
                        manifest.close();
                    }

                    LOGI("MINIRD wrote snapshot {}: {}", captureIndex, rawPath.string());
                });

            if (IsMiniRenderDocFullHdOutput(*texture))
                ++diagnosticFullHdCount;

            if (diagnosticFullHdCount >= 2 || diagnosticCaptureIndex >= 32) {
                diagnosticCaptureState = DiagnosticCaptureState::Complete;
                captureComplete = true;
                break;
            }
        }

        QueueDiagnosticSampledInputCaptures();
        diagnosticRenderTargets.clear();

        if (captureComplete) {
            FlushDiagnosticDrawTrace();
            const std::filesystem::path captureDirectory{diagnosticCaptureDirectory};
            const auto completePath{captureDirectory / "capture_complete.txt"};
            const auto captureCount{diagnosticCaptureIndex};
            pendingDiagnosticCaptureCallbacks.emplace_back([completePath, captureCount] {
                std::ofstream complete{completePath, std::ios::out | std::ios::trunc};
                if (complete)
                    complete << "capture_complete=true\nsnapshots=" << captureCount << "\n";
                LOGI("MINIRD capture complete with {} snapshots", captureCount);
            });
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
            TrackDiagnosticRenderTargets(colorAttachments);
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
                QueueDiagnosticRenderTargetCaptures();
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

        return gotoNext;
    }

    void CommandExecutor::FinishRenderPass() {
        if (renderPass) {
            slot->nodes.emplace_back(std::in_place_type_t<node::RenderPassEndNode>());
            QueueDiagnosticRenderTargetCaptures();
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
        bool didLock{view->LockWithTag(tag)};
        if (didLock) {
            // TODO: fixup remaining bugs with this and add better heuristics to avoid pauses
            // if (view->texture->FrequentlyLocked())
            attachedTextures.emplace_back(view->texture);
            // else
            //    preserveAttachedTextures.emplace_back(view->texture);
        }

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

    bool CommandExecutor::IsDiagnosticDrawTraceActive() const {
        return diagnosticCaptureArmed &&
               renderPassIndex == 8 &&
               (diagnosticCaptureState == DiagnosticCaptureState::WaitingForFrameStart ||
                diagnosticCaptureState == DiagnosticCaptureState::Capturing);
    }

    void CommandExecutor::AppendDiagnosticDrawTrace(std::string trace) {
        if (!IsDiagnosticDrawTraceActive())
            return;

        diagnosticDrawTraceLines.emplace_back(
            fmt::format("submission={} render_pass={}\n{}",
                        submissionNumber, renderPassIndex, std::move(trace)));
    }

    void CommandExecutor::TrackDiagnosticSampledInputs(span<TextureView *> sampledImages) {
        if (!IsDiagnosticDrawTraceActive() || diagnosticSampledInputsCaptured)
            return;

        for (auto *view : sampledImages) {
            if (!view)
                continue;

            std::shared_ptr<Texture> texture{view->texture};
            if (std::find_if(diagnosticSampledInputs.begin(), diagnosticSampledInputs.end(),
                             [&](const auto &existing) {
                                 return existing.get() == texture.get();
                             }) == diagnosticSampledInputs.end())
                diagnosticSampledInputs.emplace_back(std::move(texture));
        }
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
        if (!slot->nodes.empty()) {
            waiterThread.Queue(cycle, {});
            for (auto &captureCallback : pendingDiagnosticCaptureCallbacks)
                waiterThread.Queue(cycle, std::move(captureCallback));
            pendingDiagnosticCaptureCallbacks.clear();
        }

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
