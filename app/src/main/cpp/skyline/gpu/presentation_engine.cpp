// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <android/native_window_jni.h>
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
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        surfaceCondition.notify_all();
        presentQueue.Close();
        if (presentationThread.joinable())
            presentationThread.join();

        choreographerStop.store(true, std::memory_order_release);
        {
            std::scoped_lock lock{choreographerControlMutex};
            if (choreographerLooper)
                ALooper_wake(choreographerLooper);
        }
        if (choreographerThread.joinable())
            choreographerThread.join();

        // No presentation thread can submit new copies once it has joined.
        try {
            std::scoped_lock lock{mutex};
            DestroySwapchain();
            vkSurface.reset();
        } catch (const std::exception &e) {
            LOGE("Failed to drain presentation at teardown: {}", e.what());
        }

        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }

        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr))
            env->DeleteGlobalRef(jSurface);
    }

    void PresentationEngine::ChoreographerCallback(int64_t frameTimeNanos, PresentationEngine *engine) {
        const i64 previousTime{engine->lastChoreographerTime.exchange(frameTimeNanos)};
        const i64 cycleLength{frameTimeNanos - previousTime};
        const i64 currentDuration{engine->refreshCycleDuration.load()};
        if (!previousTime || (cycleLength > 0 && std::abs(cycleLength - currentDuration) > (constant::NsInMillisecond / 2))) {
            std::scoped_lock lock{engine->mutex};
            i64 duration{currentDuration};
            if (engine->window && !engine->window->perform(engine->window, NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION, &duration) && duration > 0)
                engine->refreshCycleDuration.store(duration);
            else if (previousTime && cycleLength > 0)
                engine->refreshCycleDuration.store(cycleLength);
        }

        if (!engine->skipSignal.exchange(false))
            engine->vsyncEvent->Signal();

        if (!engine->choreographerStop.load(std::memory_order_acquire))
            AChoreographer_postFrameCallback64(AChoreographer_getInstance(), reinterpret_cast<AChoreographer_frameCallback64>(&ChoreographerCallback), engine);
    }

    void PresentationEngine::ChoreographerThread() {
        if (int result{pthread_setname_np(pthread_self(), "Sky-Choreo")})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        try {
            {
                std::scoped_lock lock{choreographerControlMutex};
                choreographerLooper = ALooper_prepare(0);
            }
            AChoreographer_postFrameCallback64(AChoreographer_getInstance(), reinterpret_cast<AChoreographer_frameCallback64>(&ChoreographerCallback), this);
            while (!choreographerStop.load(std::memory_order_acquire))
                ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        } catch (const signal::SignalException &e) {
            LOGE("{}\\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));
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
        std::scoped_lock lock{choreographerControlMutex};
        choreographerLooper = nullptr;
    }

    void PresentationEngine::PresentFrame(const PresentableFrame &frame) {
        std::unique_lock lock(mutex);
        surfaceCondition.wait(lock, [this]() { return stopping || vkSurface.has_value(); });
        if (stopping)
            return;

        frame.fence.Wait(state.soc->host1x);

        // A retained display view may belong to an older guest representation.
        frame.textureView->texture->ReconcileAliases();
        std::scoped_lock textureLock(*frame.textureView);

        auto texture{frame.textureView->texture};
        if (!vkSwapchain || swapchainNeedsRecreation || frame.textureView->format != swapchainFormat || texture->dimensions != swapchainExtent)
            UpdateSwapchain(frame.textureView->format, texture->dimensions);

        int result;
        if (frame.crop != windowCrop) {
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

        // VK_SUBOPTIMAL_KHR is a successful acquisition: the semaphore and image
        // belong to this frame and must not be acquired again.
        std::pair<vk::Result, u32> nextImage;
        try {
            nextImage = vkSwapchain->acquireNextImage(std::numeric_limits<u64>::max(), *acquireSemaphore, {});
        } catch (const vk::OutOfDateKHRError &) {
            UpdateSwapchain(frame.textureView->format, texture->dimensions);
            nextImage = vkSwapchain->acquireNextImage(std::numeric_limits<u64>::max(), *acquireSemaphore, {});
        }
        if (nextImage.first == vk::Result::eErrorOutOfDateKHR) {
            UpdateSwapchain(frame.textureView->format, texture->dimensions);
            nextImage = vkSwapchain->acquireNextImage(std::numeric_limits<u64>::max(), *acquireSemaphore, {});
        }
        if (nextImage.first != vk::Result::eSuccess && nextImage.first != vk::Result::eSuboptimalKHR)
            throw exception("vkAcquireNextImageKHR returned an unhandled result '{}'", vk::to_string(nextImage.first));

        auto &nextImageTexture{images.at(nextImage.second)};
        auto &presentSemaphore{presentSemaphores[nextImage.second]};

        texture->SynchronizeHost();
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

        const i64 refreshDuration{refreshCycleDuration.load()};
        const i64 choreographerTime{lastChoreographerTime.load()};
        if (frame.swapInterval && refreshDuration > 0) {
            // If we have a swap interval, we have to adjust the timestamp to emulate the swap interval
            i64 lastFramePresentTime{util::AlignUpNpot(windowLastTimestamp, refreshDuration)};
            if (lastFramePresentTime > choreographerTime)
                // If the last frame was presented after the last choreographer callback, calculate the new frame's timestamp relative to it
                timestamp = std::max(timestamp, lastFramePresentTime + (refreshDuration * frame.swapInterval));
            else
                // If there has been a choreographer callback since the last frame, calculate the new frame's timestamp relative to it
                timestamp = std::max(timestamp, choreographerTime + (2 * refreshDuration * frame.swapInterval));
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

        vk::Result presentResult{};
        {
            std::scoped_lock queueLock{gpu.queueMutex};
            try {
                presentResult = gpu.vkQueue.presentKHR(vk::PresentInfoKHR{
                .swapchainCount = 1,
                .pSwapchains = &**vkSwapchain,
                .pImageIndices = &nextImage.second,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &*presentSemaphore,
                }); // We handle suboptimal transforms externally.
            } catch (const vk::OutOfDateKHRError &) {
                presentResult = vk::Result::eErrorOutOfDateKHR;
            }
        }
        if (presentResult == vk::Result::eErrorOutOfDateKHR)
            swapchainNeedsRecreation = true;
        else if (presentResult != vk::Result::eSuccess && presentResult != vk::Result::eSuboptimalKHR)
            throw exception("vkQueuePresentKHR returned '{}'", vk::to_string(presentResult));

        if (presentResult != vk::Result::eErrorOutOfDateKHR && !firstFramePresented.exchange(true))
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

    void PresentationEngine::DestroySwapchain() {
        if (!vkSwapchain)
            return;

        // PresentFrame submits swapchain copies directly while owning mutex.
        // Its return does not imply GPU completion, and retaining Texture wrappers
        // cannot retain the memory owned by VkSwapchainKHR. Drain those commands
        // before either surface replacement or extent/format replacement.
        {
            std::scoped_lock queueLock{gpu.queueMutex};
            gpu.vkQueue.waitIdle();
        }
        // Release the queue lock before dropping resources/fence dependencies.
        // If the wait throws, leave the old swapchain and references intact.
        images.fill(nullptr);
        frameFences.fill(nullptr);
        vkSwapchain.reset();
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

        // Surface formats must be checked after every surface replacement, even
        // when the guest requests the same format as before.
        const auto formats{gpu.vkPhysicalDevice.getSurfaceFormatsKHR(**vkSurface)};
        auto surfaceSupports{[&](vk::Format candidate) {
            return std::any_of(formats.begin(), formats.end(), [&](const vk::SurfaceFormatKHR &entry) {
                return entry.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear &&
                    (entry.format == candidate || entry.format == vk::Format::eUndefined);
            });
        }};
        texture::Format underlyingFormat{format};
        if (!surfaceSupports(*format)) {
            LOGD("Surface does not support requested format '{}', trying RGBA8", vk::to_string(*format));
            underlyingFormat = format::R8G8B8A8Unorm;
            if (!surfaceSupports(*underlyingFormat))
                throw exception("Surface supports neither requested format nor RGBA8 fallback");
        }

        constexpr vk::ImageUsageFlags presentUsage{vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
        if ((capabilities.supportedUsageFlags & presentUsage) != presentUsage)
            throw exception("Swapchain doesn't support image usage '{}': {}", vk::to_string(presentUsage), vk::to_string(capabilities.supportedUsageFlags));

        auto requestedMode{*state.settings->disableFrameThrottling ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo};
        auto modes{gpu.vkPhysicalDevice.getSurfacePresentModesKHR(**vkSurface)};
        if (std::find(modes.begin(), modes.end(), requestedMode) == modes.end())
            throw exception("Swapchain doesn't support present mode: {}", vk::to_string(requestedMode));

        vk::CompositeAlphaFlagBitsKHR compositeAlpha{vk::CompositeAlphaFlagBitsKHR::eInherit};
        if (!(capabilities.supportedCompositeAlpha & compositeAlpha)) {
            if (capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
                compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
            else if (capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied)
                compositeAlpha = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
            else if (capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePostMultiplied)
                compositeAlpha = vk::CompositeAlphaFlagBitsKHR::ePostMultiplied;
            else
                throw exception("Surface does not support a compatible composite alpha mode");
        }

        DestroySwapchain();
        vkSwapchain.emplace(gpu.vkDevice, vk::SwapchainCreateInfoKHR{
            .surface = **vkSurface,
            .minImageCount = minImageCount,
            .imageFormat = *underlyingFormat,
            .imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = presentUsage,
            .imageSharingMode = vk::SharingMode::eExclusive,
            .compositeAlpha = compositeAlpha,
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
        if (!swapchainImageCount)
            throw exception("Host returned an empty swapchain");
        swapchainNeedsRecreation = false;
    }

    void PresentationEngine::UpdateSurface(jobject newSurface) {
        std::scoped_lock guard{mutex};
        if (stopping)
            return;
        DestroySwapchain();
        vkSurface.reset();
        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }

        auto env{state.jvm->GetEnv()};
        if (!env->IsSameObject(jSurface, nullptr)) {
            env->DeleteGlobalRef(jSurface);
            jSurface = nullptr;
        }
        if (!env->IsSameObject(newSurface, nullptr))
            jSurface = env->NewGlobalRef(newSurface);

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
        }
    }

    u64 PresentationEngine::Present(const std::shared_ptr<TextureView> &texture, i64 timestamp, i64 swapInterval, AndroidRect crop, NativeWindowScalingMode scalingMode, NativeWindowTransform transform, skyline::service::hosbinder::AndroidFence fence, const std::function<void()> &presentCallback) {
        {
            std::unique_lock lock{mutex};
            surfaceCondition.wait(lock, [this] { return stopping || vkSurface.has_value(); });
            if (stopping) {
                lock.unlock();
                presentCallback();
                return 0;
            }
        }

        const auto id{nextFrameId.fetch_add(1, std::memory_order_relaxed)};
        presentQueue.Push(PresentableFrame{
            texture,
            fence,
            timestamp,
            swapInterval,
            presentCallback,
            id,
            crop,
            scalingMode,
            transform
        });

        return id;
    }

    NativeWindowTransform PresentationEngine::GetTransformHint() {
        std::unique_lock lock{mutex};
        surfaceCondition.wait(lock, [this]() { return stopping || vkSurface.has_value(); });
        if (stopping)
            return NativeWindowTransform::Identity;
        return GetAndroidTransform(vkSurfaceCapabilities.currentTransform);
    }
}
