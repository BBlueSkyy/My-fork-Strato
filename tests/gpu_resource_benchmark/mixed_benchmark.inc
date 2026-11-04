// SPDX-License-Identifier: MPL-2.0
// Included only in an archived mixed test build, never in production.
void CheckMixedResourceBenchmark(GPU &gpu, bool formatList) {
    using namespace resource_bench;
    constexpr size_t size{6 * 4096};
    auto *bytes{static_cast<u8 *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    assert(bytes != MAP_FAILED);
    std::unique_ptr<u8, std::function<void(u8 *)>> memory{bytes, [](auto *p) { munmap(p, size); }};
    std::fill(bytes, bytes + size, 0x2D);
    MixedManagerCleanup cleanup{gpu};
    MixedExecutor operation{gpu};
    auto image{gpu.texture.FindOrCreate(PartialDescriptor(bytes + 4096))};
    operation.Read(image.get()); operation.Submit();
    auto view{gpu.buffer.FindOrCreate({bytes + 8192, 8192})};
    auto plain{gpu.buffer.FindOrCreate({bytes + 16384, 4096})};
    operation.Read(view); operation.Read(plain); operation.Read(image.get()); operation.Submit();
    assert(view.GetBuffer()->regionTracked && !plain.GetBuffer()->regionTracked);
    const auto group{view.GetBuffer()->regionAuthority.load()};
    const auto submissions{gpu.scheduler.submissions};
    const auto images{gpu.memory.count}, staging{gpu.memory.stagingCount};
    const auto traps{gpu.state.process->trap.changes.load()};
    auto waits = [&] {
        unsigned value{};
        for (const auto &cycle : gpu.scheduler.cycles) value += cycle->gpuWaits + cycle->submitWaits;
        return value;
    };
    const auto previousWaits{waits()};
    auto *dispatcher{const_cast<vk::raii::DeviceDispatcher *>(gpu.vkDevice.getDispatcher())};
    dispatcher->vkCmdCopyImage = CopyImage;
    dispatcher->vkCmdCopyBufferToImage = BufferImage;
    dispatcher->vkCmdCopyImageToBuffer = ImageBuffer;
    dispatcher->vkCmdCopyBuffer = CopyBuffer;
    dispatcher->vkCmdPipelineBarrier = Barrier;
    auto bufferRead = [&](BufferView buffer) {
        operation.Run([&] {
            operation.executor.AttachBuffer(buffer);
            auto publication{operation.executor.AcquireRegionPublication({})};
        });
    };
    Measure("unshared_buffer_prepare", formatList, [&] { bufferRead(plain); });
    Measure("mixed_buffer_cached_prepare", formatList, [&] { bufferRead(view); });
    image->lockOnAttach = true;
    Measure("mixed_image_cached_prepare", formatList, [&] {
        operation.Run([&] {
            operation.executor.AttachTexture(image.get());
            auto publication{operation.executor.AcquireRegionPublication({})};
        });
    });
    assert(gpu.scheduler.submissions == submissions && gpu.memory.count == images &&
        gpu.memory.stagingCount == staging && gpu.state.process->trap.changes.load() == traps &&
        waits() == previousWaits && view.GetBuffer()->regionAuthority.load() == group);
    dispatcher->vkCmdCopyImage = imageCopy;
    dispatcher->vkCmdCopyBufferToImage = bufferImage;
    dispatcher->vkCmdCopyImageToBuffer = imageBuffer;
    dispatcher->vkCmdCopyBuffer = bufferCopy;
    dispatcher->vkCmdPipelineBarrier = pipelineBarrier;
    operation.Submit();
    gpu.scheduler.Complete();
    assert(!validationErrors);
}
