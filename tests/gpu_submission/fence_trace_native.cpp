#include <utility>
#include <gpu/fence_cycle.h>
#include <future>
#include <iostream>
#include <cstring>
#include "trace_clock.h"
using u8 = uint8_t;
using namespace std::chrono_literals;
namespace bp = skyline::gpu::buffer_readback_probe;
static void Check(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
#include "../buffer_trap_dependencies/vulkan.inc"
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
    {
        NativeBuffer backing{vk}, output{vk};
        VkEvent event{};
        VkEventCreateInfo ei{};
        ei.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        VkCheck(vkCreateEvent(vk.device, &ei, nullptr, &event));
        VkFence fence{};
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkCheck(vkCreateFence(vk.device, &fi, nullptr, &fence));
        auto cmd = vk.Begin();
        vkCmdWaitEvents(cmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0,
                        nullptr, 0, nullptr);
        vkCmdFillBuffer(cmd, backing.buffer, 0, 4096, 0x64646464);
        Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{0, 0, 4096};
        vkCmdCopyBuffer(cmd, backing.buffer, output.buffer, 1, &copy);
        Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        VkCheck(vkEndCommandBuffer(cmd));
        {
            skyline::gpu::FenceCycle cycle{device, vk::Fence{fence}, {}};
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            VkCheck(vkQueueSubmit(vk.queue, 1, &si, fence));
            cycle.NotifySubmitted();
            bp::WaitTrace trace{18, 4096, true};
            std::promise<void> phaseEntered;
            auto future = std::async(std::launch::async, [&] { trace_test::Wait(cycle, trace, phaseEntered, 5); });
            phaseEntered.get_future().get();
            const bool blocked = future.wait_for(50ms) == std::future_status::timeout;
            VkCheck(vkSetEvent(vk.device, event));
            future.get();
            trace.Finish();
            Check(blocked, "native producer dependency skipped");
            Check(trace.parts[bp::Driver] >= 25000000, "native driver wait not attributed");
            for (size_t i{}; i < 4096; ++i)
                Check(output.data[i] == 0x64, "CPU readback observed stale producer data");
            output.data[37] = 0x9a;
            Check(output.data[0] == 0x64 && output.data[38] == 0x64, "partial CPU store lost preserved bytes");
        }
        vk.Retire(cmd, fence);
        vkDestroyEvent(vk.device, event, nullptr);
        Check(validationErrors == 0, "native synchronization validation failed");
    }
    std::cout << "PASS actual FenceCycle native Vulkan gated writer, GPU copy/readback, sampled driver wait and "
                 "preserved bytes; zero validation errors\n";
}
