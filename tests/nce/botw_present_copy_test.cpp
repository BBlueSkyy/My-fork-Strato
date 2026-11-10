// SPDX-License-Identifier: MPL-2.0
// Executes the extracted production Texture::CopyFrom body. Vulkan descriptors
// are real pinned Vulkan-Hpp; the scheduler, fence and recorder are host facades.
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "../../app/src/main/cpp/skyline/common/botw_pixel_probe.h"

namespace boost::container {
template<class T, std::size_t> using small_vector = std::vector<T>;
}

namespace {
void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Handle, class Native> Handle HandleAt(std::uintptr_t value) {
    return Handle{reinterpret_cast<Native>(value)};
}
struct RecordedCommand {
    enum Kind { Barrier, Copy, Blit, Readback } kind;
    vk::PipelineStageFlags sourceStage{}, destinationStage{};
    std::vector<vk::ImageMemoryBarrier> images;
    std::vector<vk::BufferMemoryBarrier> buffers;
    vk::Image source{}, destination{};
    vk::Buffer buffer{};
    std::vector<vk::BufferImageCopy> regions;
};
struct Recording {
    std::vector<RecordedCommand> events;
    std::vector<std::pair<vk::Image, vk::ImageLayout>> layouts;
    vk::ImageLayout &Layout(vk::Image image) {
        for (auto &[key, layout] : layouts)
            if (key == image) return layout;
        throw std::runtime_error("unregistered image");
    }
};
}

namespace vk::raii {
class CommandBuffer {
    Recording &recording;
  public:
    explicit CommandBuffer(Recording &recording) : recording{recording} {}
    void pipelineBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination,
                         vk::DependencyFlags, vk::ArrayProxy<const vk::MemoryBarrier>,
                         vk::ArrayProxy<const vk::BufferMemoryBarrier> buffers,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> images) {
        RecordedCommand event{RecordedCommand::Barrier};
        event.sourceStage = source;
        event.destinationStage = destination;
        event.buffers.assign(buffers.begin(), buffers.end());
        event.images.assign(images.begin(), images.end());
        for (const auto &barrier : images) {
            Check(recording.Layout(barrier.image) == barrier.oldLayout, "barrier old layout differs from recorded image state");
            recording.Layout(barrier.image) = barrier.newLayout;
        }
        recording.events.push_back(std::move(event));
    }
    void ImageTransfer(RecordedCommand::Kind kind, vk::Image source, vk::ImageLayout sourceLayout,
                       vk::Image destination, vk::ImageLayout destinationLayout) {
        Check(recording.Layout(source) == sourceLayout, "copy source layout mismatch");
        Check(recording.Layout(destination) == destinationLayout, "copy destination layout mismatch");
        RecordedCommand event{kind};
        event.source = source;
        event.destination = destination;
        recording.events.push_back(std::move(event));
    }
    void copyImage(vk::Image source, vk::ImageLayout sourceLayout, vk::Image destination,
                   vk::ImageLayout destinationLayout, const vk::ImageCopy &) {
        ImageTransfer(RecordedCommand::Copy, source, sourceLayout, destination, destinationLayout);
    }
    void blitImage(vk::Image source, vk::ImageLayout sourceLayout, vk::Image destination,
                   vk::ImageLayout destinationLayout, const vk::ImageBlit &, vk::Filter filter) {
        Check(filter == vk::Filter::eLinear, "present blit filter changed");
        ImageTransfer(RecordedCommand::Blit, source, sourceLayout, destination, destinationLayout);
    }
    void copyImageToBuffer(vk::Image source, vk::ImageLayout layout, vk::Buffer buffer,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) {
        Check(recording.Layout(source) == layout && layout == vk::ImageLayout::eTransferSrcOptimal,
              "readback image is not in transfer-source layout");
        RecordedCommand event{RecordedCommand::Readback};
        event.source = source;
        event.buffer = buffer;
        event.regions.assign(regions.begin(), regions.end());
        recording.events.push_back(std::move(event));
    }
};
}

namespace skyline::gpu {
using i32 = std::int32_t;
template<class T> struct span {
    span() = default;
    explicit span(T &) {}
    span(std::vector<T> &) {}
};
using exception = std::runtime_error;
struct FenceCycle {
    bool signalled{};
    std::vector<std::shared_ptr<void>> dependencies;
    void WaitSubmit() {}
    template<class F> std::shared_ptr<FenceCycle> RecordSemaphoreWaitUsage(F &&record) {
        return record(vk::Semaphore{});
    }
    template<class... Objects> void AttachObjects(Objects &&...objects) {
        (dependencies.emplace_back(objects), ...);
    }
    void Signal() { signalled = true; dependencies.clear(); }
};
namespace memory {
struct StagingBuffer {
    vk::Buffer vkBuffer;
};
}
#include "present_readback.inc"
namespace texture {
using Dimensions = vk::Extent3D;
enum class Format { Rgba, Srgb };
}
struct Scheduler {
    Recording &recording;
    std::vector<std::shared_ptr<FenceCycle>> submissions;
    template<class F> std::shared_ptr<FenceCycle> Submit(F &&record, span<vk::Semaphore>, span<vk::Semaphore>) {
        vk::raii::CommandBuffer commandBuffer{recording};
        record(commandBuffer);
        auto cycle{std::make_shared<FenceCycle>()};
        submissions.push_back(cycle);
        return cycle;
    }
};
struct GPU {
    Recording recording;
    Scheduler scheduler{recording, {}};
};
struct Texture : std::enable_shared_from_this<Texture> {
    GPU &gpu;
    vk::Image backing;
    texture::Format format{texture::Format::Rgba};
    texture::Dimensions dimensions{1920, 1080, 1};
    vk::ImageLayout layout;
    std::uint32_t layerCount{1}, levelCount{1};
    std::shared_ptr<FenceCycle> cycle;
    Texture(GPU &gpu, std::uintptr_t image, vk::ImageLayout layout)
        : gpu{gpu}, backing{HandleAt<vk::Image, VkImage>(image)}, layout{layout} {
        gpu.recording.layouts.emplace_back(backing, layout);
    }
    void WaitOnBacking() {}
    void WaitOnFence() {}
    vk::Image GetBacking() { return backing; }
    void CopyFrom(std::shared_ptr<Texture> source, vk::Semaphore waitSemaphore,
                  vk::Semaphore signalSemaphore, texture::Format srcFormat,
                  const vk::ImageSubresourceRange &subresource,
                  const PresentPixelReadback *pixelReadback);
};
#define TRACE_EVENT(...) ((void)0)
#include "present_copy.inc"
#undef TRACE_EVENT
}

namespace {
using namespace skyline::gpu;
const vk::ImageSubresourceRange range{
    .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1,
};
void RunCase(bool readback, bool blit, bool backingFormatsEqual,
             vk::ImageLayout sourceLayout, vk::ImageLayout destinationLayout) {
    GPU gpu;
    auto source{std::make_shared<Texture>(gpu, 1, sourceLayout)};
    auto destination{std::make_shared<Texture>(gpu, 2, destinationLayout)};
    source->format = backingFormatsEqual ? texture::Format::Rgba : texture::Format::Srgb;
    // Exercise the source-cycle submission path too.
    source->cycle = std::make_shared<FenceCycle>();
    PresentPixelReadback request{
        std::make_shared<memory::StagingBuffer>(HandleAt<vk::Buffer, VkBuffer>(3)),
        std::make_shared<memory::StagingBuffer>(HandleAt<vk::Buffer, VkBuffer>(4)),
    };
    std::weak_ptr<memory::StagingBuffer> before{request.source}, after{request.destination};
    // A view can differ from the source backing. CopyFrom receives the view
    // format; the PE metadata must describe the operation actually recorded.
    const auto swapchainFormat{blit ? texture::Format::Srgb : texture::Format::Rgba};
    destination->CopyFrom(source, {}, {}, swapchainFormat, range, readback ? &request : nullptr);
    const auto &events{gpu.recording.events};
    const bool recordedCopy{std::any_of(events.begin(), events.end(), [](const auto &event) {
        return event.kind == RecordedCommand::Copy;
    })};
    const auto nextImageTexture{destination};
    struct PendingMetadata {
        texture::Format sourceFormat, destinationFormat;
        bool usesImageCopy;
    } pending{
        source->format, destination->format,
#include "uses_image_copy.inc"
    };
    const auto *pixelPending{&pending};
#include "raw_comparable.inc"
    Check(comparable == (recordedCopy && source->format == destination->format),
          backingFormatsEqual && blit
              ? "PE treats equal backings as raw-comparable despite a mismatched view recording a blit"
              : "PE raw comparability disagrees with the recorded operation and backing formats");
    Check(std::count_if(events.begin(), events.end(), [](const auto &event) { return event.kind == RecordedCommand::Copy; }) == !blit,
          "incorrect image-copy selection");
    Check(std::count_if(events.begin(), events.end(), [](const auto &event) { return event.kind == RecordedCommand::Blit; }) == blit,
          "incorrect image-blit selection");
    Check(std::count_if(events.begin(), events.end(), [](const auto &event) { return event.kind == RecordedCommand::Readback; }) == (readback ? 2 : 0),
          "incorrect number of readback commands");
    if (!readback)
        Check(std::none_of(events.begin(), events.end(), [](const auto &event) {
            return !event.buffers.empty() || event.destinationStage == vk::PipelineStageFlagBits::eHost;
        }), "normal present recorded diagnostic host visibility barriers");
    Check(gpu.recording.Layout(source->backing) == sourceLayout, "source layout was not restored");
    const auto restoredDestination{destinationLayout == vk::ImageLayout::eUndefined
        ? vk::ImageLayout::eTransferDstOptimal : destinationLayout};
    Check(gpu.recording.Layout(destination->backing) == restoredDestination, "destination layout was not restored");
    if (readback) {
        bool visibility{}, destinationDependency{};
        std::size_t readbacks{};
        for (const auto &event : events) {
            if (event.kind == RecordedCommand::Barrier) {
                for (const auto &barrier : event.images)
                    if (barrier.image == destination->backing && barrier.oldLayout == vk::ImageLayout::eTransferDstOptimal &&
                        barrier.newLayout == vk::ImageLayout::eTransferSrcOptimal) {
                        Check(event.sourceStage == vk::PipelineStageFlagBits::eTransfer &&
                              event.destinationStage == vk::PipelineStageFlagBits::eTransfer &&
                              barrier.srcAccessMask == vk::AccessFlagBits::eTransferWrite &&
                              barrier.dstAccessMask == vk::AccessFlagBits::eTransferRead,
                              "destination readback lacks transfer write-to-read dependency");
                        destinationDependency = true;
                    }
                if (!event.buffers.empty()) {
                    Check(readbacks == 2 && event.buffers.size() == 2 &&
                          event.sourceStage == vk::PipelineStageFlagBits::eTransfer &&
                          event.destinationStage == vk::PipelineStageFlagBits::eHost,
                          "host visibility barrier does not follow both readbacks");
                    for (std::size_t index{}; index < event.buffers.size(); ++index) {
                        const auto &barrier{event.buffers[index]};
                        Check(barrier.buffer == (index ? request.destination->vkBuffer : request.source->vkBuffer) &&
                              barrier.srcAccessMask == vk::AccessFlagBits::eTransferWrite &&
                              barrier.dstAccessMask == vk::AccessFlagBits::eHostRead &&
                              barrier.offset == 0 && barrier.size == VK_WHOLE_SIZE,
                              "readback buffer host visibility dependency is incomplete");
                    }
                    visibility = true;
                }
            }
            if (event.kind != RecordedCommand::Readback) continue;
            Check(event.source == (readbacks ? destination->backing : source->backing), "readback order/image mismatch");
            if (readbacks) Check(destinationDependency, "destination readback precedes its transfer dependency");
            Check(event.buffer == (readbacks ? request.destination->vkBuffer : request.source->vkBuffer), "readback buffer mismatch");
            Check(event.regions.size() == 4, "readback must contain exactly four patches");
            constexpr std::array<std::pair<int, int>, 4> positions{{{480,270}, {1440,270}, {480,810}, {1440,810}}};
            for (std::size_t index{}; index < event.regions.size(); ++index) {
                const auto &region{event.regions[index]};
                Check(region.bufferOffset == index * 1024 && region.bufferRowLength == 0 && region.bufferImageHeight == 0 &&
                      region.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eColor &&
                      region.imageSubresource.mipLevel == 0 && region.imageSubresource.baseArrayLayer == 0 &&
                      region.imageSubresource.layerCount == 1 && region.imageOffset.x == positions[index].first &&
                      region.imageOffset.y == positions[index].second && region.imageOffset.z == 0 &&
                      region.imageExtent == vk::Extent3D{16,16,1}, "readback patch region differs from expected packed 16x16 patch");
            }
            ++readbacks;
        }
        Check(visibility && destinationDependency, "missing readback synchronization");
    }
    // Keep an unsignalled cycle owned externally, as scheduler/frameFences do.
    auto completion{destination->cycle};
    request = {};
    if (readback) {
        Check(!completion->signalled && !before.expired() && !after.expired(),
              "staging buffers freed while the present-copy fence is still unsignalled");
    } else {
        Check(before.expired() && after.expired(), "normal present retained unused diagnostic staging");
    }
    completion->Signal();
    Check(before.expired() && after.expired(), "staging buffers retained after fence dependency release");
}
}

int main() {
    try {
        for (const bool readback : {false, true})
            for (const bool blit : {false, true})
                for (const bool backingFormatsEqual : {false, true})
                    for (const auto destinationLayout : {vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eUndefined})
                        for (const auto sourceLayout : {vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal})
                            RunCase(readback, blit, backingFormatsEqual, sourceLayout, destinationLayout);
        std::cout << "PASS: 32 production present-copy/PE metadata cases; copy/blit, comparability, patches, layouts, visibility, staging lifetime (host facades; no Vulkan driver)\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
