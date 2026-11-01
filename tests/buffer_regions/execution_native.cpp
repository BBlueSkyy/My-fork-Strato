// SPDX-License-Identifier: MPL-2.0
#include <utility>
#include <gpu/fence_cycle.h>
#include <future>
#include <cstdio>
#include <cstring>
using u8 = uint8_t;
using namespace std::chrono_literals;
static void Check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Fixture {
    static constexpr size_t Size = 8192;
};
#include "vulkan.inc"
#include "host_barrier.inc"
int main() {
    vk::raii::Context context;
    vk::raii::Instance instance{nullptr};
    vk::raii::PhysicalDevice physical{nullptr};
    vk::raii::Device device{nullptr};
    Vulkan vk;
    instance = vk::raii::Instance{context, vk.instance};
    physical = vk::raii::PhysicalDevice{instance, vk.physical};
    device = vk::raii::Device{physical, vk.device};
    vk.externalDeviceInstanceOwnership = true;
    NativeBuffer backing(vk), output(vk);
    VkFence fa{}, fb{};
    VkFenceCreateInfo fc{};
    fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkCheck(vkCreateFence(vk.device, &fc, nullptr, &fa));
    VkCheck(vkCreateFence(vk.device, &fc, nullptr, &fb));
    VkSemaphore semaphore{};
    VkSemaphoreCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkCheck(vkCreateSemaphore(vk.device, &sc, nullptr, &semaphore));
    VkEvent event{};
    VkEventCreateInfo ec{};
    ec.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkCheck(vkCreateEvent(vk.device, &ec, nullptr, &event));
    auto a = vk.Begin();
    vkCmdFillBuffer(a, backing.buffer, 0, Fixture::Size, 0x6a6a6a6a);
    vk::raii::CommandBuffer viewA{device, a, vk.pool};
    RecordStagedReadbackBarrier(viewA);
    VkCheck(vkEndCommandBuffer(a));
    auto b = vk.Begin();
    vkCmdWaitEvents(b, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    Barrier(b, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{0, 0, Fixture::Size};
    vkCmdCopyBuffer(b, backing.buffer, output.buffer, 1, &copy);
    vk::raii::CommandBuffer viewB{device, b, vk.pool};
    RecordStagedReadbackBarrier(viewB);
    VkCheck(vkEndCommandBuffer(b));
    {
        auto writer = std::make_shared<skyline::gpu::FenceCycle>(device, vk::Fence{fa}, vk::Semaphore{semaphore});
        auto consumer = std::make_shared<skyline::gpu::FenceCycle>(device, vk::Fence{fb}, vk::Semaphore{});
        VkSubmitInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        ai.commandBufferCount = 1;
        ai.pCommandBuffers = &a;
        ai.signalSemaphoreCount = 1;
        ai.pSignalSemaphores = &semaphore;
        VkCheck(vkQueueSubmit(vk.queue, 1, &ai, fa));
        writer->NotifySubmitted();
        writer->RecordSemaphoreWaitUsage([&](vk::Semaphore sem) {
            VkSemaphore wait = sem;
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            bi.waitSemaphoreCount = 1;
            bi.pWaitSemaphores = &wait;
            bi.pWaitDstStageMask = &stage;
            bi.commandBufferCount = 1;
            bi.pCommandBuffers = &b;
            VkCheck(vkQueueSubmit(vk.queue, 1, &bi, fb));
            consumer->NotifySubmitted();
            return consumer;
        });
        auto retained = std::make_shared<int>(1);
        std::weak_ptr<int> lifetime = retained;
        writer->AttachObject(retained);
        retained.reset();
        auto retirement = std::async(std::launch::async, [&] { writer->Wait(true); });
        auto data = std::async(std::launch::async, [&] { return writer->WaitForExecution(); });
        const bool available = data.wait_for(1s) == std::future_status::ready;
        if (!available) {
            vkSetEvent(vk.device, event);
            data.get();
            retirement.get();
            Check(false, "data wait blocked behind later Vulkan consumer");
        }
        Check(data.get(), "producer execution was canceled");
        const bool stillPending = vkGetFenceStatus(vk.device, fb) == VK_NOT_READY && !writer->Poll() && !lifetime.expired();
        for (size_t i = 0; i < Fixture::Size; ++i)
            Check(backing.data[i] == 0x6a, "real producer readback byte mismatch");
        VkCheck(vkSetEvent(vk.device, event));
        retirement.get();
        Check(stillPending && lifetime.expired(), "data read changed semaphore/dependency retirement");
        for (size_t i = 0; i < Fixture::Size; ++i)
            Check(output.data[i] == 0x6a, "later GPU consumer readback byte mismatch");
    }
    VkCheck(vkWaitForFences(vk.device, 1, &fb, true, UINT64_MAX));
    VkCheck(vkWaitForFences(vk.device, 1, &fa, true, UINT64_MAX));
    vkDestroyFence(vk.device, fb, nullptr);
    vkDestroyFence(vk.device, fa, nullptr);
    vkDestroyEvent(vk.device, event, nullptr);
    vkDestroySemaphore(vk.device, semaphore, nullptr);
    Check(!validationErrors, "native execution/retirement validation error");
    std::puts("PASS actual FenceCycle+production HOST_READ barrier: producer data available while semaphore consumer is pending; 8192 bytes match; "
              "lifetime retires only afterward; zero sync validation errors");
}
