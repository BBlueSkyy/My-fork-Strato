// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <android/native_window_jni.h>
#include <android/choreographer.h>
#include <common/settings.h>
#include <common/signal.h>
#include <jvm.h>
#include <gpu.h>
#include <os.h>
#include <gpu/diagnostic/ue4_ui_capture.h>
#include <filesystem>
#include <fstream>
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
        // Diagnostic only: manual four-frame source capture before host swapchain copy.
        // Deliberately independent of Title ID and the programmable GPU pipeline.
        {
            namespace fs = std::filesystem;
            auto &session{diagnostic::UiSurfaceCapture()};
            static std::uint32_t pollCount{};
            static std::uint32_t remainingFrames{};
            static bool waitingForWhiteLogo{};
            static bool waitingDrawSteps{};
            static std::uint32_t logoPresentCounter{};
            static fs::path captureDirectory{};
            const fs::path root{fs::path(state.os->publicAppFilesPath) / "gpu_capture" / "ue4_ui"};

            if (!remainingFrames && !waitingForWhiteLogo && (++pollCount % 16 == 1)) {
                std::error_code ec;
                fs::create_directories(root, ec);
                if (!ec) {
                    const auto armPath{root / "ARM_CAPTURE"};
                    if (fs::exists(armPath, ec) && !ec) {
                        ec.clear();
                        fs::remove(armPath, ec);
                        if (!ec) {
                            for (unsigned index{1}; index <= 999; ++index) {
                                const auto candidate{root / fmt::format("capture_{:03}", index)};
                                ec.clear();
                                if (fs::exists(candidate, ec) || ec)
                                    continue;
                                if (!fs::create_directories(candidate, ec) || ec)
                                    continue;
                                captureDirectory = candidate;
                                const bool drawSteps{fs::exists(root / "ENABLE_UI_DRAW_STEPS", ec) && !ec};
                                ec.clear();
                                const bool logoTrigger{fs::exists(root / "ENABLE_BOOT_LOGO_TRIGGER", ec) && !ec};
                                ec.clear();
                                if (logoTrigger) {
                                    waitingForWhiteLogo = true;
                                    waitingDrawSteps = drawSteps;
                                    logoPresentCounter = 0;
                                } else {
                                    remainingFrames = diagnostic::Ue4UiCaptureState::FrameCount;
                                    session.Start(candidate.string(), drawSteps);
                                }
                                std::ofstream readme(candidate / "README.txt");
                                if (readme) {
                                    readme << "Strato UE4 UI surface mini-RenderDoc (diagnostic only)\n"
                                           << "4 consecutive presented frames; the first may have been rendered before arming.\n"
                                           << "present_source_XX.raw is a linear host GPU readback, not a PNG.\n"
                                           << "gpu_events.txt contains bounded draw, shader, sampler and TIC diagnostics.\n"
                                           << "present_index_approx is approximate due to asynchronous rendering.\n"
                                           << "UI draw-step mode is optional; ENABLE_UI_DRAW_STEPS must exist BEFORE ARM_CAPTURE.\n"
                                           << "ENABLE_BOOT_LOGO_TRIGGER waits for a mostly white VI source before starting the 4-frame capture.\n"
                                           << "When enabled it splits selected passes for GPU-ordered intermediate snapshots.\n"
                                           << "No guest memory or rendering semantics changed outside capture.\n";
                                }
                                LOGI("MINIRD_UE4_UI armed: {} logo_trigger={}", candidate.string(), logoTrigger);
                                break;
                            }
                        }
                    } else {
                        const auto hint{root / "ARM_CAPTURE.rename_this_file_when_ready"};
                        ec.clear();
                        if (!fs::exists(hint, ec) && !ec) {
                            std::ofstream instructions(hint);
                            if (instructions)
                                instructions << "Open the affected UE4 menu, rename this file to ARM_CAPTURE and return to Strato.\n";
                        }
                    }
                }
            }


            // Opt-in splash/logo trigger: avoid spending all four presented
            // frames on the black boot interval. Probe the already synchronized
            // VI source every 8 presentations, at most 320 presentations.
            // This path adds waits/copies ONLY while the manual trigger exists.
            if (waitingForWhiteLogo) {
                ++logoPresentCounter;
                if (logoPresentCounter > 320) {
                    LOGW("MINIRD_UE4_UI logo trigger timed out after 320 presentations");
                    std::ofstream timeout(captureDirectory / "logo_trigger_timeout.txt");
                    if (timeout) timeout << "No qualifying white presentation source observed.\\n";
                    waitingForWhiteLogo = false;
                } else if ((logoPresentCounter % 8) == 0 &&
                    (texture->format->vkFormat == vk::Format::eR8G8B8A8Unorm ||
                     texture->format->vkFormat == vk::Format::eB8G8R8A8Unorm) &&
                    texture->layout != vk::ImageLayout::eUndefined &&
                    texture->levelCount == 1 && texture->layerCount == 1 &&
                    texture->sampleCount == vk::SampleCountFlagBits::e1 &&
                    texture->surfaceSize > 0 && texture->surfaceSize <= 12ULL * 1024 * 1024) {
                    if (texture->cycle)
                        texture->cycle->Wait();
                    auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
                    auto sampleCycle{gpu.scheduler.Submit(
                        [texture, staging](vk::raii::CommandBuffer &cmd) {
                            texture->CopyIntoStagingBuffer(cmd, staging);
                        })};
                    sampleCycle->AttachObjects(texture, staging);
                    sampleCycle->Wait();
                    // R/B order does not matter for detecting near-white pixels.
                    const auto pixelCount{std::min<std::size_t>(
                        static_cast<std::size_t>(texture->dimensions.width) * texture->dimensions.height,
                        staging->size() / 4)};
                    const auto *rgba{static_cast<const std::uint8_t *>(staging->data())};
                    std::size_t samples{}, white{};
                    for (std::size_t pixel{}; pixel < pixelCount; pixel += 701) {
                        const auto offset{pixel * 4};
                        ++samples;
                        if (rgba[offset] >= 205 && rgba[offset + 1] >= 205 && rgba[offset + 2] >= 205)
                            ++white;
                    }
                    // Require a substantial white area, not white animation
                    // specks on black as observed in the previous capture.
                    if (samples >= 100 && white * 5 >= samples) {
                        LOGI("MINIRD_UE4_UI white logo detected: {}/{} sampled pixels, {} presented frames since arm",
                             white, samples, logoPresentCounter);
                        session.Start(captureDirectory.string(), waitingDrawSteps, true);
                        remainingFrames = diagnostic::Ue4UiCaptureState::FrameCount;
                        waitingForWhiteLogo = false;
                    }
                }
            }

            if (remainingFrames) {
                const auto captureIndex{diagnostic::Ue4UiCaptureState::FrameCount - remainingFrames};
                session.presentIndex.store(captureIndex, std::memory_order_release);
                uintptr_t guestMap{};
                size_t guestSize{};
                if (texture->guest) {
                    if (!texture->guest->mappings.empty())
                        guestMap = reinterpret_cast<uintptr_t>(texture->guest->mappings.front().data());
                    for (const auto &mapping : texture->guest->mappings) {
                        guestSize += mapping.size();
                        // Watch actual physical spans, not a guessed contiguous range.
                        // Direct Maxwell color-target writers may use the very same
                        // guest backing as the VI image presented to Android.
                        if (!mapping.empty())
                            session.WatchMapping(reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
                    }
                }
                const auto validReadback{
                    texture->layout != vk::ImageLayout::eUndefined &&
                    texture->levelCount == 1 && texture->layerCount == 1 &&
                    texture->sampleCount == vk::SampleCountFlagBits::e1 &&
                    texture->surfaceSize > 0 && texture->surfaceSize <= 12ULL * 1024 * 1024 &&
                    (texture->format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
                    !texture->format->IsCompressed()
                };

                std::ofstream meta(captureDirectory / fmt::format("present_{:02}.txt", captureIndex));
                if (meta)
                    meta << fmt::format(
                        "frame={} frame_id={} host={}x{} format={} layout={} size={} "
                        "guest_map=0x{:X} guest_map_bytes={} texture=0x{:X} "
                        "crop={},{},{},{} transform={} scaling={} eligible={}\n",
                        captureIndex, frame.id, texture->dimensions.width, texture->dimensions.height,
                        vk::to_string(texture->format->vkFormat),
                        vk::to_string(texture->layout), texture->surfaceSize,
                        guestMap, guestSize, reinterpret_cast<uintptr_t>(texture.get()),
                        frame.crop.left, frame.crop.top, frame.crop.right, frame.crop.bottom,
                        static_cast<u32>(frame.transform), static_cast<u32>(frame.scalingMode),
                        validReadback);

                session.Record(fmt::format(
                    "PRESENT_SOURCE frame_index={} frame_id={} source_texture=0x{:X} "
                    "guest_map=0x{:X} guest_bytes={} source_cycle=0x{:X} dimensions={}x{} eligible={}",
                    captureIndex, frame.id, reinterpret_cast<uintptr_t>(texture.get()),
                    guestMap, guestSize, reinterpret_cast<uintptr_t>(texture->cycle.get()),
                    texture->dimensions.width, texture->dimensions.height, validReadback));

                if (validReadback) {
                    // Same proven readback path as earlier mini-RenderDoc diagnostic PRs.
                    // Wait only on this explicit, opt-in diagnostic path.
                    if (texture->cycle)
                        texture->cycle->Wait();
                    auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
                    auto diagnosticCycle{gpu.scheduler.Submit(
                        [texture, staging](vk::raii::CommandBuffer &commandBuffer) {
                            texture->CopyIntoStagingBuffer(commandBuffer, staging);
                        })};
                    diagnosticCycle->AttachObjects(texture, staging);
                    diagnosticCycle->Wait();
                    const auto fileName{fmt::format("present_source_{:02}_{}x{}_{}.raw",
                        captureIndex, texture->dimensions.width, texture->dimensions.height,
                        vk::to_string(texture->format->vkFormat))};
                    std::ofstream raw(captureDirectory / fileName, std::ios::binary | std::ios::trunc);
                    if (raw) {
                        raw.write(reinterpret_cast<const char *>(staging->data()),
                                  static_cast<std::streamsize>(staging->size()));
                        LOGI("MINIRD_UE4_UI wrote frame {}: {}", captureIndex, fileName);
                    } else {
                        LOGE("MINIRD_UE4_UI could not write frame {}", captureIndex);
                    }
                } else {
                    LOGW("MINIRD_UE4_UI frame {} skipped: unsupported source layout/size", captureIndex);
                }
                if (--remainingFrames == 0) {
                    session.Finish();
                    std::ofstream complete(captureDirectory / "capture_complete.txt");
                    if (complete)
                        complete << "capture_complete=true\nframes=4\n";
                    LOGI("MINIRD_UE4_UI capture complete");
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
