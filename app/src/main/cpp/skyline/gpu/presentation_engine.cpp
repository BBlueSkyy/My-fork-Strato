// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <android/native_window_jni.h>
#include <array>
#include <deque>
#include <optional>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <gpu/diagnostic/cosmic_shake_pass.h>
#include <os.h>
#include <android/choreographer.h>
#include <common/settings.h>
#include <common/signal.h>
#include <jvm.h>
#include <gpu.h>
#include <soc.h>
#include <loader/loader.h>
#include <kernel/types/KProcess.h>
#include "presentation_engine.h"
#include "native_window.h"
#include "texture/format.h"

extern jint Fps;
extern jfloat AverageFrametimeMs;
extern jfloat AverageFrametimeDeviationMs;

namespace skyline::gpu {
    using namespace service::hosbinder;

    PresentationEngine::PresentationEngine(const DeviceState &state, GPU &gpu)
        : state{state},
          gpu{gpu},
          presentSemaphores{util::MakeFilledArray<vk::raii::Semaphore, MaxSwapchainImageCount>(gpu.vkDevice, vk::SemaphoreCreateInfo{})},
          acquireSemaphores{util::MakeFilledArray<vk::raii::Semaphore, MaxSwapchainImageCount>(gpu.vkDevice, vk::SemaphoreCreateInfo{})},
          presentationTrack{static_cast<u64>(trace::TrackIds::Presentation), perfetto::ProcessTrack::Current()},
          vsyncEvent{std::make_shared<kernel::type::KEvent>(state, true)},
          choreographerThread{&PresentationEngine::ChoreographerThread, this},
          presentationThread{&PresentationEngine::PresentationThread, this} {
        auto desc{presentationTrack.Serialize()};
        desc.set_name("Presentation");
        perfetto::TrackEvent::SetTrackDescriptor(presentationTrack, desc);
    }

    PresentationEngine::~PresentationEngine() {
        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr))
            env->DeleteGlobalRef(jSurface);

        if (choreographerThread.joinable()) {
            if (choreographerLooper) {
                choreographerStop = true;
                ALooper_wake(choreographerLooper);
            }
            choreographerThread.join();
        }
    }

    void PresentationEngine::ChoreographerCallback(int64_t frameTimeNanos, PresentationEngine *engine) {
        // If the duration of this cycle deviates by ±0.5ms from the current refresh cycle duration then we reevaluate it
        i64 cycleLength{frameTimeNanos - engine->lastChoreographerTime};
        if (std::abs(cycleLength - engine->refreshCycleDuration) > (constant::NsInMillisecond / 2)) {
            if (engine->window)
                engine->window->perform(engine->window, NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION, &engine->refreshCycleDuration);
            else
                engine->refreshCycleDuration = cycleLength;
        }

        // Record the current cycle's timestamp and signal the V-Sync event to notify the game that a frame has been displayed
        engine->lastChoreographerTime = frameTimeNanos;
        if (!engine->skipSignal.exchange(false))
            engine->vsyncEvent->Signal();

        // Post the frame callback to be triggered on the next display refresh
        AChoreographer_postFrameCallback64(AChoreographer_getInstance(), reinterpret_cast<AChoreographer_frameCallback64>(&ChoreographerCallback), engine);
    }

    void PresentationEngine::ChoreographerThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-Choreo")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            choreographerLooper = ALooper_prepare(0);
            AChoreographer_postFrameCallback64(AChoreographer_getInstance(), reinterpret_cast<AChoreographer_frameCallback64>(&ChoreographerCallback), this);
            while (!choreographerStop)
                ALooper_pollOnce(-1, nullptr, nullptr, nullptr); // Will block and process callbacks till ALooper_wake() is called with choreographerStop set
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

    void PresentationEngine::PresentFrame(const PresentableFrame &frame) {
        std::unique_lock lock(mutex);
        surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });

        frame.fence.Wait(state.soc->host1x);

        // A retained display view may belong to an older guest representation.
        frame.textureView->texture->ReconcileAliases();
        std::scoped_lock textureLock(*frame.textureView);

        auto texture{frame.textureView->texture};
        if (frame.textureView->format != swapchainFormat || texture->dimensions != swapchainExtent)
            UpdateSwapchain(frame.textureView->format, texture->dimensions);

        int result;
        if (frame.crop && frame.crop != windowCrop) {
            if ((result = window->perform(window, NATIVE_WINDOW_SET_CROP, &frame.crop)))
                throw exception("Setting the layer crop to ({}-{})x({}-{}) failed with {}", frame.crop.left, frame.crop.right, frame.crop.top, frame.crop.bottom, result);
            windowCrop = frame.crop;
        }

        if (frame.scalingMode != NativeWindowScalingMode::Freeze && windowScalingMode != frame.scalingMode) {
            if ((result = window->perform(window, NATIVE_WINDOW_SET_SCALING_MODE, static_cast<i32>(frame.scalingMode))))
                throw exception("Setting the layer scaling mode to '{}' failed with {}", ToString(frame.scalingMode), result);
            windowScalingMode = frame.scalingMode;
        }

        if ((result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TRANSFORM, static_cast<i32>(frame.transform))))
            throw exception("Setting the buffer transform to '{}' failed with {}", ToString(frame.transform), result);
        windowTransform = frame.transform;

        auto &acquireSemaphore{acquireSemaphores[frameIndex]};
        auto &frameFence{frameFences[frameIndex]};
        if (frameFence)
            frameFence->Wait();

        frameIndex = (frameIndex + 1) % swapchainImageCount;

        std::pair<vk::Result, u32> nextImage;
        while (nextImage = vkSwapchain->acquireNextImage(std::numeric_limits<u64>::max(), *acquireSemaphore, {}), nextImage.first != vk::Result::eSuccess) [[unlikely]] {
            if (nextImage.first == vk::Result::eSuboptimalKHR)
                surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });
            else
                throw exception("vkAcquireNextImageKHR returned an unhandled result '{}'", vk::to_string(nextImage.first));
        }

        auto &nextImageTexture{images.at(nextImage.second)};
        auto &presentSemaphore{presentSemaphores[nextImage.second]};

        texture->SynchronizeHost();

        // Opt-in Cosmic Shake auto capture. Sample only 9 4x4 patches of the *presented*
        // texture. A circular in-memory history keeps the preceding Texman/pass
        // decisions; full pass readbacks begin only after a suspected blackout.
        {
            namespace fs = std::filesystem;
            auto &capture{diagnostic::CosmicShakePass()};
            const fs::path root{fs::path(state.os->publicAppFilesPath) / "gpu_capture" / "cosmic_shake"};
            static u32 pollCount{};
            static bool justArmed{};
            static bool autoCaptureUsed{};
            static std::array<u32, 9> previousSamples{};
            static bool havePrevious{};
            static u64 previousFrame{};
            static u32 previousWidth{}, previousHeight{};

            struct PendingProbe {
                std::shared_ptr<memory::StagingBuffer> staging;
                std::shared_ptr<FenceCycle> cycle;
                u64 frame{};
                u32 width{}, height{};
                uintptr_t source{};
            };
            static std::deque<PendingProbe> pending;
            static constexpr size_t MaxPendingProbes{8};

            auto startCapture{[&](const char *reason, const std::string &detail = {}) {
                std::error_code ec;
                fs::create_directories(root, ec);
                if (ec)
                    return false;
                for (unsigned index{1}; index <= 999; ++index) {
                    const auto dir{root / fmt::format("capture_{:03}", index)};
                    ec.clear();
                    if (fs::exists(dir, ec) || ec)
                        continue;
                    if (!fs::create_directories(dir, ec) || ec)
                        continue;
                    capture.Start(dir.string());
                    justArmed = true;
                    std::ofstream readme(dir / "README.txt");
                    if (readme)
                        readme << "Cosmic Shake automatic/manual pass-boundary capture: " << reason << '\n'
                               << "pretrigger_* are circular metadata histories, not GPU readbacks.\n"
                               << "The screen probe is a 3x3 RGB brightness heuristic, not proof of a bad frame.\n"
                               << "Full pass readbacks now select 1280x720 focused composition or 1920x1080 outputs only.\n"
                               << "The screen detector reads the source before swapchain CopyFrom, not the Android swapchain.\n"
                               << "pretrigger_present_sources.txt maps the presenting texture to preceding pass metadata.\n"
                               << "Focused 1280x720 scene passes now get paired pre_pass_NNN.txt (before beginRenderPass) and pass_NNN.txt (after endRenderPass).\n"
                               << "The paired readback keeps four draws within the original render pass; it does not inspect per-draw outputs.\n"
                               << "Pre-pass transfer adds an image memory dependency and may mask a Vulkan race; compare with normal monitoring.\n"
                               << "Other 1920x1080 passes remain post-only; pair IDs refer to the same texture/mip/layer and submission.\n"
                               << "Pass readbacks remain bounded and can perturb race timing.\n"
                               << detail << '\n';
                    LOGI("COSMIC_SHAKE_CAPTURE trigger={} directory={}", reason, dir.string());
                    return true;
                }
                return false;
            }};

            // Retire probes in presentation order. An eight-entry in-flight
            // queue tolerates normal GPU latency without missing every other frame.
            // Never block on a diagnostic fence.
            while (!pending.empty() && pending.front().cycle->Poll(false)) {
                auto completed{std::move(pending.front())};
                pending.pop_front();
                const auto result{vmaInvalidateAllocation(completed.staging->vmaAllocator,
                    completed.staging->vmaAllocation, 0, completed.staging->size())};
                if (result == VK_SUCCESS) {
                    std::array<u32, 9> values{};
                    const auto *data{static_cast<const u8 *>(completed.staging->data())};
                    u32 sum{}, darkSites{}, brightToDark{}, oldSum{};
                    for (size_t i{}; i < values.size(); ++i) {
                        u32 rgb{};
                        for (size_t pixel{}; pixel < 16; ++pixel) {
                            const auto *p{data + i * 64 + pixel * 4};
                            rgb += static_cast<u32>(p[0]) + p[1] + p[2];
                        }
                        values[i] = rgb / 48; // Ignore alpha, invariant to RGBA/BGRA.
                        sum += values[i];
                        oldSum += previousSamples[i];
                        darkSites += values[i] <= 22;
                        brightToDark += previousSamples[i] >= 65 && values[i] <= 22;
                    }
                    const auto mean{sum / 9}, previousMean{oldSum / 9};
                    const bool comparable{havePrevious &&
                        previousWidth == completed.width && previousHeight == completed.height &&
                        completed.frame > previousFrame && completed.frame - previousFrame <= 4};
                    const bool suspectedBlackout{comparable &&
                        (brightToDark >= 5 || (brightToDark >= 3 && darkSites >= 6 &&
                         previousMean >= 60 && mean * 3 < previousMean))};
                    std::string observation{fmt::format(
                        "probe_frame={} source=0x{:X} extent={}x{} mean_rgb={} previous_mean={} "
                        "dark_sites={} bright_to_dark={} comparable={} trigger={} values=",
                        completed.frame, completed.source, completed.width, completed.height,
                        mean, previousMean, darkSites, brightToDark, comparable, suspectedBlackout)};
                    for (size_t i{}; i < values.size(); ++i)
                        observation += fmt::format("{}{}", i ? "," : "", values[i]);
                    capture.RecordScreenProbe(observation);

                    previousSamples = values;
                    havePrevious = true;
                    previousFrame = completed.frame;
                    previousWidth = completed.width;
                    previousHeight = completed.height;

                    if (suspectedBlackout && capture.monitoring.load(std::memory_order_acquire) &&
                        !capture.active.load(std::memory_order_acquire) && !autoCaptureUsed &&
                        startCapture("auto_blackout", observation)) {
                        autoCaptureUsed = true;
                        capture.RecordPresent("TRIGGER " + observation);
                    }
                } else {
                    capture.RecordScreenProbe(fmt::format("probe_frame={} invalidate_error={}",
                        completed.frame, static_cast<int>(result)));
                    havePrevious = false;
                }
            }

            // Existing ARM_CAPTURE still works; AUTO_CAPTURE arms monitoring
            // once, without having to catch an individual flicker manually.
            if (!capture.RecordingMetadata() && (++pollCount % 8 == 1)) {
                std::error_code ec;
                fs::create_directories(root, ec);
                if (!ec) {
                    const auto manual{root / "ARM_CAPTURE"};
                    const auto automatic{root / "AUTO_CAPTURE"};
                    ec.clear();
                    if (fs::exists(manual, ec) && !ec) {
                        ec.clear();
                        if (fs::remove(manual, ec) && !ec)
                            startCapture("manual");
                    } else if (!autoCaptureUsed) {
                        ec.clear();
                        if (fs::exists(automatic, ec) && !ec) {
                            ec.clear();
                            if (fs::remove(automatic, ec) && !ec) {
                                havePrevious = false;
                                capture.EnableMonitoring();
                                LOGI("COSMIC_SHAKE_CAPTURE automatic monitoring armed");
                            }
                        }
                    }
                    for (const auto &name : {
                        "ARM_CAPTURE.rename_this_file_when_ready",
                        "AUTO_CAPTURE.rename_this_file_when_ready"}) {
                        const auto hint{root / name};
                        ec.clear();
                        if (!fs::exists(hint, ec) && !ec) {
                            std::ofstream out(hint);
                            if (out)
                                out << "Rename to " << (name[0] == 'A' && name[1] == 'U'
                                    ? "AUTO_CAPTURE" : "ARM_CAPTURE")
                                    << " when ready to arm a Cosmic Shake diagnostic.\n";
                        }
                    }
                }
            }

            if (capture.RecordingMetadata()) {
                const auto frameNumber{capture.monitorPresent.fetch_add(1, std::memory_order_acq_rel) + 1};
                uintptr_t sourceGuestMap{};
                if (texture->guest && !texture->guest->mappings.empty())
                    sourceGuestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                capture.RecordPrePresent(fmt::format(
                    "SOURCE_BEFORE_SWAPCHAIN_COPY monitor_present={} texture=0x{:X} "
                    "guest_map=0x{:X} dimensions={}x{} format={} view_mip={}+{} "
                    "view_layer={}+{} layout={}",
                    frameNumber, reinterpret_cast<uintptr_t>(texture.get()), sourceGuestMap,
                    texture->dimensions.width, texture->dimensions.height,
                    vk::to_string(texture->format->vkFormat),
                    frame.textureView->range.baseMipLevel, frame.textureView->range.levelCount,
                    frame.textureView->range.baseArrayLayer, frame.textureView->range.layerCount,
                    vk::to_string(texture->layout)));
                if (capture.active.load(std::memory_order_acquire)) {
                    capture.RecordPresent(fmt::format(
                        "PRESENT monitor_present={} approx_present={} texture=0x{:X} "
                        "dimensions={}x{} format={} layout={} just_armed={} "
                        "source_guest_map=0x{:X} source_view_mip={}+{} source_view_layer={}+{} "
                        "sampled_before_swapchain_copy=1",
                        frameNumber, capture.presentIndex.load(std::memory_order_relaxed),
                        reinterpret_cast<uintptr_t>(texture.get()), texture->dimensions.width,
                        texture->dimensions.height, vk::to_string(texture->format->vkFormat),
                        vk::to_string(texture->layout), justArmed,
                        sourceGuestMap, frame.textureView->range.baseMipLevel,
                        frame.textureView->range.levelCount,
                        frame.textureView->range.baseArrayLayer,
                        frame.textureView->range.layerCount));
                    if (!justArmed) {
                        const auto next{capture.presentIndex.fetch_add(1, std::memory_order_acq_rel) + 1};
                        if (next >= diagnostic::CosmicShakePassCapture::FrameCount) {
                            capture.Stop();
                            LOGI("COSMIC_SHAKE_CAPTURE capture complete");
                        }
                    }
                    justArmed = false;
                }

                const auto vkFormat{texture->format->vkFormat};
                const bool supported{
                    vkFormat == vk::Format::eR8G8B8A8Unorm ||
                    vkFormat == vk::Format::eR8G8B8A8Srgb ||
                    vkFormat == vk::Format::eB8G8R8A8Unorm ||
                    vkFormat == vk::Format::eB8G8R8A8Srgb
                };
                if (pending.size() < MaxPendingProbes && capture.RecordingMetadata() && supported &&
                    texture->dimensions.width >= 8 && texture->dimensions.height >= 8 &&
                    texture->sampleCount == vk::SampleCountFlagBits::e1 &&
                    texture->layout != vk::ImageLayout::eUndefined && texture->GetBacking() &&
                    (texture->usage & vk::ImageUsageFlagBits::eTransferSrc)) {
                    constexpr size_t SampleBytes{9 * 4 * 4 * 4};
                    auto staging{gpu.memory.AllocateStagingBuffer(SampleBytes)};
                    // The previous GPU writer must be submitted before the probe.
                    if (texture->cycle)
                        texture->cycle->WaitSubmit();
                    const auto oldLayout{texture->layout};
                    auto probeCycle{gpu.scheduler.Submit([&, staging](vk::raii::CommandBuffer &commandBuffer) {
                        const vk::ImageSubresourceRange subresource{
                            .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1
                        };
                        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                            vk::ImageMemoryBarrier{
                                .image = texture->GetBacking(),
                                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                                .oldLayout = oldLayout,
                                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .subresourceRange = subresource,
                            });
                        std::array<vk::BufferImageCopy, 9> areas{};
                        for (u32 i{}; i < areas.size(); ++i) {
                            const auto x{std::min(texture->dimensions.width - 4,
                                texture->dimensions.width * ((i % 3) + 1) / 4)};
                            const auto y{std::min(texture->dimensions.height - 4,
                                texture->dimensions.height * ((i / 3) + 1) / 4)};
                            areas[i] = vk::BufferImageCopy{
                                .bufferOffset = i * 64,
                                .imageSubresource = {
                                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                                    .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1
                                },
                                .imageOffset = vk::Offset3D{static_cast<i32>(x), static_cast<i32>(y), 0},
                                .imageExtent = vk::Extent3D{4, 4, 1},
                            };
                        }
                        commandBuffer.copyImageToBuffer(texture->GetBacking(),
                            vk::ImageLayout::eTransferSrcOptimal, staging->vkBuffer,
                            vk::ArrayProxy(static_cast<u32>(areas.size()), areas.data()));
                        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eHost, {}, {}, vk::BufferMemoryBarrier{
                                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                .dstAccessMask = vk::AccessFlagBits::eHostRead,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .buffer = staging->vkBuffer, .offset = 0, .size = SampleBytes,
                            }, {});
                        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                            vk::ImageMemoryBarrier{
                                .image = texture->GetBacking(),
                                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                                .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
                                    vk::AccessFlagBits::eMemoryWrite,
                                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                                .newLayout = oldLayout,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .subresourceRange = subresource,
                            });
                    })};
                    probeCycle->AttachObjects(texture, staging);
                    // Preserve ordering against the existing presentation copy.
                    texture->cycle = probeCycle;
                    pending.emplace_back(PendingProbe{
                        std::move(staging), std::move(probeCycle), frameNumber,
                        texture->dimensions.width, texture->dimensions.height,
                        reinterpret_cast<uintptr_t>(texture.get())
                    });
                }
            }
        }

        nextImageTexture->CopyFrom(texture, *acquireSemaphore, *presentSemaphore, swapchainFormat, vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        });

        frameFence = nextImageTexture->cycle;

        auto getMonotonicNsNow{[]() -> i64 {
            timespec time;
            if (clock_gettime(CLOCK_MONOTONIC, &time))
                throw exception("Failed to clock_gettime with '{}'", strerror(errno));
            return (time.tv_sec * constant::NsInSecond) + time.tv_nsec;
        }};

        i64 timestamp{frame.timestamp};
        if (timestamp) {
            // If the timestamp is specified, we need to convert it from the util::GetTimeNs base to the CLOCK_MONOTONIC one
            // We do so by getting an offset from the current time in nanoseconds and then adding it to the current time in CLOCK_MONOTONIC
            // Note: It's important we do this right before present as going past the timestamp could lead to fewer Binder IPC calls
            i64 current{util::GetTimeNs()};
            if (current < timestamp) {
                timestamp = getMonotonicNsNow() + (timestamp - current);
            } else {
                timestamp = 0;
            }
        }

        if (frame.swapInterval) {
            // If we have a swap interval, we have to adjust the timestamp to emulate the swap interval
            i64 lastFramePresentTime{util::AlignUpNpot(windowLastTimestamp, refreshCycleDuration)};
            if (lastFramePresentTime > lastChoreographerTime)
                // If the last frame was presented after the last choreographer callback, calculate the new frame's timestamp relative to it
                timestamp = std::max(timestamp, lastFramePresentTime + (refreshCycleDuration * frame.swapInterval));
            else
                // If there has been a choreographer callback since the last frame, calculate the new frame's timestamp relative to it
                timestamp = std::max(timestamp, lastChoreographerTime + (2 * refreshCycleDuration * frame.swapInterval));
        }

        i64 lastTimestamp{std::exchange(windowLastTimestamp, timestamp)};
        if (!timestamp && lastTimestamp)
            // We need to nullify the timestamp if it transitioned from being specified (non-zero) to unspecified (zero)
            timestamp = NativeWindowTimestampAuto;

        if (timestamp && (result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP, timestamp)))
            throw exception("Setting the buffer timestamp to {} failed with {}", timestamp, result);

        u64 frameId{};
        if ((result = window->perform(window, NATIVE_WINDOW_GET_NEXT_FRAME_ID, &frameId)))
            throw exception("Retrieving the next frame's ID failed with {}", result);

        {
            std::scoped_lock queueLock{gpu.queueMutex};
            std::ignore = gpu.vkQueue.presentKHR(vk::PresentInfoKHR{
                .swapchainCount = 1,
                .pSwapchains = &**vkSwapchain,
                .pImageIndices = &nextImage.second,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &*presentSemaphore,
            }); // We don't care about suboptimal images as they are caused by not respecting the transform hint, we handle transformations externally
        }

        if (!firstFramePresented.exchange(true))
            state.jvm->NotifyFirstFramePresented();

        timestamp = (timestamp && !*state.settings->disableFrameThrottling) ? timestamp : getMonotonicNsNow(); // We tie FPS to the submission time rather than presentation timestamp, if we don't have the presentation timestamp available or if frame throttling is disabled as we want the maximum measured FPS to not be restricted to the refresh rate
        if (frameTimestamp) {
            i64 sampleWeight{Fps ? Fps : 1}; //!< The weight of each sample in calculating the average, we want to roughly average the past second

            auto weightedAverage{[](auto weight, auto previousAverage, auto current) {
                return (((weight - 1) * previousAverage) + current) / weight;
            }}; //!< Modified moving average (https://en.wikipedia.org/wiki/Moving_average#Modified_moving_average)

            i64 currentFrametime{timestamp - frameTimestamp};
            averageFrametimeNs = weightedAverage(sampleWeight, averageFrametimeNs, currentFrametime);
            AverageFrametimeMs = static_cast<jfloat>(averageFrametimeNs) / constant::NsInMillisecond;

            i64 currentFrametimeDeviation{std::abs(averageFrametimeNs - currentFrametime)};
            averageFrametimeDeviationNs = weightedAverage(sampleWeight, averageFrametimeDeviationNs, currentFrametimeDeviation);
            AverageFrametimeDeviationMs = static_cast<jfloat>(averageFrametimeDeviationNs) / constant::NsInMillisecond;

            Fps = static_cast<jint>(std::round(static_cast<float>(constant::NsInSecond) / static_cast<float>(averageFrametimeNs)));

            TRACE_EVENT_INSTANT("gpu", "Present", presentationTrack, "FrameTimeNs", timestamp - frameTimestamp, "Fps", Fps);

            frameTimestamp = timestamp;
        } else {
            frameTimestamp = timestamp;
        }
    }

    void PresentationEngine::PresentationThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-Present")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            presentQueue.Process([this](const PresentableFrame &frame) {
                PresentFrame(frame);
                frame.presentCallback(); // We're calling the callback here as it's outside of all the locks in PresentFrame
                skipSignal = true;
                vsyncEvent->Signal();
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

    NativeWindowTransform GetAndroidTransform(vk::SurfaceTransformFlagBitsKHR transform) {
        using NativeWindowTransform = NativeWindowTransform;
        switch (transform) {
            case vk::SurfaceTransformFlagBitsKHR::eIdentity:
            case vk::SurfaceTransformFlagBitsKHR::eInherit:
                return NativeWindowTransform::Identity;
            case vk::SurfaceTransformFlagBitsKHR::eRotate90:
                return NativeWindowTransform::Rotate90;
            case vk::SurfaceTransformFlagBitsKHR::eRotate180:
                return NativeWindowTransform::Rotate180;
            case vk::SurfaceTransformFlagBitsKHR::eRotate270:
                return NativeWindowTransform::Rotate270;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirror:
                return NativeWindowTransform::MirrorHorizontal;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate90:
                return NativeWindowTransform::MirrorHorizontalRotate90;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate180:
                return NativeWindowTransform::MirrorVertical;
            case vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate270:
                return NativeWindowTransform::MirrorVerticalRotate90;
        }
    }

    void PresentationEngine::UpdateSwapchain(texture::Format format, texture::Dimensions extent) {
        auto minImageCount{std::max(vkSurfaceCapabilities.minImageCount, *state.settings->forceTripleBuffering ? 3U : 2U)};
        if (minImageCount > MaxSwapchainImageCount)
            throw exception("Requesting swapchain with higher image count ({}) than maximum slot count ({})", minImageCount, MaxSwapchainImageCount);

        const auto &capabilities{vkSurfaceCapabilities};
        if (minImageCount < capabilities.minImageCount || (capabilities.maxImageCount && minImageCount > capabilities.maxImageCount))
            throw exception("Cannot update swapchain to accomodate image count: {} ({}-{})", minImageCount, capabilities.minImageCount, capabilities.maxImageCount);
        else if (capabilities.minImageExtent.height > extent.height || capabilities.minImageExtent.width > extent.width || capabilities.maxImageExtent.height < extent.height || capabilities.maxImageExtent.width < extent.width)
            throw exception("Cannot update swapchain to accomodate image extent: {}x{} ({}x{}-{}x{})", extent.width, extent.height, capabilities.minImageExtent.width, capabilities.minImageExtent.height, capabilities.maxImageExtent.width, capabilities.maxImageExtent.height);

        vk::Format vkFormat{*format};
        texture::Format underlyingFormat{format};
        if (swapchainFormat != format) {
            auto formats{gpu.vkPhysicalDevice.getSurfaceFormatsKHR(**vkSurface)};
            if (std::find(formats.begin(), formats.end(), vk::SurfaceFormatKHR{vkFormat, vk::ColorSpaceKHR::eSrgbNonlinear}) == formats.end()) {
                LOGD("Surface doesn't support requested image format '{}' with colorspace '{}'", vk::to_string(vkFormat), vk::to_string(vk::ColorSpaceKHR::eSrgbNonlinear));
                underlyingFormat = format::R8G8B8A8Unorm;
            }
        }

        constexpr vk::ImageUsageFlags presentUsage{vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
        if ((capabilities.supportedUsageFlags & presentUsage) != presentUsage)
            throw exception("Swapchain doesn't support image usage '{}': {}", vk::to_string(presentUsage), vk::to_string(capabilities.supportedUsageFlags));

        auto requestedMode{*state.settings->disableFrameThrottling ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo};
        auto modes{gpu.vkPhysicalDevice.getSurfacePresentModesKHR(**vkSurface)};
        if (std::find(modes.begin(), modes.end(), requestedMode) == modes.end())
            throw exception("Swapchain doesn't support present mode: {}", vk::to_string(requestedMode));

        vkSwapchain.emplace(gpu.vkDevice, vk::SwapchainCreateInfoKHR{
            .surface = **vkSurface,
            .minImageCount = minImageCount,
            .imageFormat = *underlyingFormat,
            .imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = presentUsage,
            .imageSharingMode = vk::SharingMode::eExclusive,
            .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eInherit,
            .presentMode = requestedMode,
            .clipped = true,
        });

        auto vkImages{vkSwapchain->getImages()};
        if (vkImages.size() > MaxSwapchainImageCount)
            throw exception("Swapchain has higher image count ({}) than maximum slot count ({})", minImageCount, MaxSwapchainImageCount);

        for (size_t index{}; index < vkImages.size(); index++) {
            auto &slot{images[index]};
            slot = std::make_shared<Texture>(*state.gpu, vkImages[index], extent, underlyingFormat, vk::ImageLayout::eUndefined, vk::ImageTiling::eOptimal, vk::ImageCreateFlags{}, presentUsage);
            slot->TransitionLayout(vk::ImageLayout::ePresentSrcKHR);
        }
        for (size_t index{vkImages.size()}; index < MaxSwapchainImageCount; index++)
            // We need to clear all the slots which aren't filled, keeping around stale slots could lead to issues
            images[index] = {};

        swapchainFormat = format;
        swapchainExtent = extent;
        swapchainImageCount = vkImages.size();
    }

    void PresentationEngine::UpdateSurface(jobject newSurface) {
        std::scoped_lock guard{mutex};

        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr)) {
            env->DeleteGlobalRef(jSurface);
            jSurface = nullptr;
        }
        if (!env->IsSameObject(newSurface, nullptr))
            jSurface = env->NewGlobalRef(newSurface);

        vkSwapchain.reset();

        if (jSurface) {
            window = ANativeWindow_fromSurface(env, jSurface);
            vkSurface.emplace(gpu.vkInstance, vk::AndroidSurfaceCreateInfoKHR{
                .window = window,
            });
            if (!gpu.vkPhysicalDevice.getSurfaceSupportKHR(gpu.vkQueueFamilyIndex, **vkSurface))
                throw exception("Vulkan Queue doesn't support presentation with surface");
            vkSurfaceCapabilities = gpu.vkPhysicalDevice.getSurfaceCapabilitiesKHR(**vkSurface);

            if (swapchainExtent && swapchainFormat)
                UpdateSwapchain(swapchainFormat, swapchainExtent);

            if (window->common.magic != AndroidNativeWindowMagic)
                throw exception("ANativeWindow* has unexpected magic: {} instead of {}", span(&window->common.magic, 1).as_string(true), span<const u8>(reinterpret_cast<const u8 *>(&AndroidNativeWindowMagic), sizeof(u32)).as_string(true));
            if (window->common.version != sizeof(ANativeWindow))
                throw exception("ANativeWindow* has unexpected version: {} instead of {}", window->common.version, sizeof(ANativeWindow));

            int result;
            if (windowCrop && (result = window->perform(window, NATIVE_WINDOW_SET_CROP, &windowCrop)))
                throw exception("Setting the layer crop to ({}-{})x({}-{}) failed with {}", windowCrop.left, windowCrop.right, windowCrop.top, windowCrop.bottom, result);

            if (windowScalingMode != NativeWindowScalingMode::ScaleToWindow && (result = window->perform(window, NATIVE_WINDOW_SET_SCALING_MODE, static_cast<i32>(windowScalingMode))))
                throw exception("Setting the layer scaling mode to '{}' failed with {}", ToString(windowScalingMode), result);

            if (windowTransform != NativeWindowTransform::Identity && (result = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_TRANSFORM, static_cast<i32>(windowTransform))))
                throw exception("Setting the buffer transform to '{}' failed with {}", ToString(windowTransform), result);

            if ((result = window->perform(window, NATIVE_WINDOW_ENABLE_FRAME_TIMESTAMPS, true)))
                throw exception("Enabling frame timestamps failed with {}", result);

            surfaceCondition.notify_all();
        } else {
            vkSurface.reset();
            window = nullptr;
        }
    }

    u64 PresentationEngine::Present(const std::shared_ptr<TextureView> &texture, i64 timestamp, i64 swapInterval, AndroidRect crop, NativeWindowScalingMode scalingMode, NativeWindowTransform transform, skyline::service::hosbinder::AndroidFence fence, const std::function<void()> &presentCallback) {
        if (!vkSurface.has_value()) {
            // We want this function to generally (not necessarily always) block when a surface is not present to implicitly pause the game
            std::unique_lock lock{mutex};
            surfaceCondition.wait(lock, [this] { return vkSurface.has_value(); });
        }

        presentQueue.Push(PresentableFrame{
            texture,
            fence,
            timestamp,
            swapInterval,
            presentCallback,
            nextFrameId,
            crop,
            scalingMode,
            transform
        });

        return nextFrameId++;
    }

    NativeWindowTransform PresentationEngine::GetTransformHint() {
        if (!vkSurface.has_value()) {
            std::unique_lock lock{mutex};
            surfaceCondition.wait(lock, [this]() { return vkSurface.has_value(); });
        }

        return GetAndroidTransform(vkSurfaceCapabilities.currentTransform);
    }
}
