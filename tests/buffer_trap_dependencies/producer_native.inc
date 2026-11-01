static void NativeWriterThenPendingReader() {
    Vulkan vk;
    NativeBuffer backing(vk), output(vk);
    Fixture f;
    auto b = f.buffer;
    b->backing->external = backing.data;
    auto writerCmd = vk.Begin();
    vkCmdFillBuffer(writerCmd, backing.buffer, 0, 4096, 0x64646464);
    Barrier(writerCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    auto writerFence = vk.Submit(writerCmd);
    VkEvent event{};
    VkEventCreateInfo ec{};
    ec.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkCheck(vkCreateEvent(vk.device, &ec, nullptr, &event));
    auto readerCmd = vk.Begin();
    vkCmdWaitEvents(readerCmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0,
                    nullptr, 0, nullptr);
    VkBufferCopy copy{0, 0, 4096};
    vkCmdCopyBuffer(readerCmd, backing.buffer, output.buffer, 1, &copy);
    Barrier(readerCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    auto readerFence = vk.Submit(readerCmd);
    auto producer = std::make_shared<gpu::FenceCycle>();
    auto reader = std::make_shared<gpu::FenceCycle>();
    producer->complete = [&] { VkCheck(vkWaitForFences(vk.device, 1, &writerFence, true, UINT64_MAX)); };
    reader->complete = [&] { VkCheck(vkWaitForFences(vk.device, 1, &readerFence, true, UINT64_MAX)); };
    AttachProducerAndReader(f, producer, reader);
    VkCheck(vkWaitForFences(vk.device, 1, &writerFence, true, UINT64_MAX));
    auto store = std::async(std::launch::async, [&] { f.Write(37, 0x9a); });
    const bool independent = store.wait_for(200ms) == std::future_status::ready;
    if (!independent) {
        vkSetEvent(vk.device, event);
        store.get();
        vk.Retire(readerCmd, readerFence);
        vk.Retire(writerCmd, writerFence);
        vkDestroyEvent(vk.device, event, nullptr);
        Check(false, "native producer readback waited for a later GPU reader");
    }
    store.get();
    const bool pending = vkGetFenceStatus(vk.device, readerFence) == VK_NOT_READY;
    const bool untouched = backing.data[37] == 0x64;
    Check(reader->waits == 0 && b->cycle == reader, "native readback discarded reader cycle");
    auto upload = std::async(std::launch::async, [&] {
        std::scoped_lock lock{*b};
        b->SynchronizeHost();
    });
    const bool uploadBlocked = upload.wait_for(100ms) == std::future_status::timeout;
    VkCheck(vkSetEvent(vk.device, event));
    upload.get();
    vk.Retire(readerCmd, readerFence);
    vk.Retire(writerCmd, writerFence);
    vkDestroyEvent(vk.device, event, nullptr);
    Check(pending && untouched && uploadBlocked, "native backing mutation skipped pending reader");
    for (size_t i{}; i < 4096; ++i)
        Check(output.data[i] == 0x64, "pending GPU reader saw later CPU bytes");
    auto cmd = vk.Begin();
    Barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyBuffer(cmd, backing.buffer, output.buffer, 1, &copy);
    Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    auto fence = vk.Submit(cmd);
    vk.Retire(cmd, fence);
    for (size_t i{}; i < 4096; ++i)
        Check(output.data[i] == (i == 37 ? 0x9a : 0x64), "next GPU reader lost updated bytes");
    Check(!validationErrors, "native producer/readback synchronization validation error");
    std::puts("PASS native GPU producer->readback overlaps pending GPU reader; upload retains reader fence; next GPU "
              "read sees correct partial store; zero validation errors");
}
