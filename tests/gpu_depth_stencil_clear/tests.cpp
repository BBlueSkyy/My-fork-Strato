// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
using u32 = uint32_t;
struct GPU {};
struct RenderPassNode {
    std::vector<vk::AttachmentReference> attachmentReferences{{.attachment = 0}};
    std::vector<vk::SubpassDescription> subpassDescriptions{{}};
    std::vector<vk::AttachmentDescription> attachmentDescriptions{{}};
    std::vector<vk::ClearValue> clearValues;
    template<typename T> static T *RebasePointer(std::vector<T> &values, const T *offset) {
        return values.data() + reinterpret_cast<uintptr_t>(offset) / sizeof(T);
    }
    bool ClearDepthStencilAttachment(const vk::ClearDepthStencilValue &, GPU &);
    bool ClearColorAttachment(u32, const vk::ClearColorValue &, GPU &);
};
#include "production.inc"

static void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    GPU gpu;
    const vk::ClearDepthStencilValue value{.depth = 0.375f, .stencil = 0x5A};
    // Full-view clears of each supported aspect combination must configure the
    // Vulkan operation for every aspect present in the attachment format.
    for (auto format : {vk::Format::eD32Sfloat, vk::Format::eS8Uint, vk::Format::eD24UnormS8Uint}) {
        RenderPassNode pass;
        pass.attachmentDescriptions[0].format = format;
        Check(pass.ClearDepthStencilAttachment(value, gpu), "first full clear was not folded");
        const auto &attachment = pass.attachmentDescriptions[0];
        if (format != vk::Format::eS8Uint)
            Check(attachment.loadOp == vk::AttachmentLoadOp::eClear, "depth load operation did not clear");
        if (format != vk::Format::eD32Sfloat)
            Check(attachment.stencilLoadOp == vk::AttachmentLoadOp::eClear, "stencil load operation did not clear");
        Check(attachment.storeOp == vk::AttachmentStoreOp::eStore &&
              attachment.stencilStoreOp == vk::AttachmentStoreOp::eStore, "clear discarded attachment contents");
        Check(pass.clearValues.at(0).depthStencil == value, "clear value was not retained");
        Check(pass.ClearDepthStencilAttachment(value, gpu), "identical full clear cannot reuse load operation");
        Check(!pass.ClearDepthStencilAttachment({.depth = 0.5f, .stencil = 3}, gpu),
              "different clear overwrote an existing load operation");
        Check(pass.clearValues.at(0).depthStencil == value, "fallback changed prior clear value");
    }
    // A prior subpass reference requires a command clear at the current point.
    RenderPassNode used;
    used.attachmentReferences.push_back({.attachment = 0});
    used.subpassDescriptions.back().pDepthStencilAttachment = reinterpret_cast<vk::AttachmentReference *>(sizeof(vk::AttachmentReference));
    Check(!used.ClearDepthStencilAttachment(value, gpu), "prior attachment usage was cleared retroactively");
    Check(used.clearValues.empty(), "fallback populated a load clear value");
    // Independently configured operations cannot be replaced by a full clear:
    // leave partial-aspect clears to vkCmdClearAttachments at their true order.
    for (bool depthClear : {false, true}) {
        RenderPassNode partial;
        auto &attachment = partial.attachmentDescriptions[0];
        if (depthClear) attachment.loadOp = vk::AttachmentLoadOp::eClear;
        else attachment.stencilLoadOp = vk::AttachmentLoadOp::eClear;
        partial.clearValues.resize(1);
        partial.clearValues[0].depthStencil = value;
        Check(!partial.ClearDepthStencilAttachment(value, gpu), "partial-aspect load operation was treated as a full clear");
    }
    const vk::ClearColorValue color{std::array<u32, 4>{11, 22, 33, 44}};
    for (bool depthFirst : {false, true}) {
        for (bool depthLastAttachment : {true, false}) {
            RenderPassNode pass;
            const u32 depthIndex = depthLastAttachment ? 1 : 0;
            const u32 colorIndex = depthLastAttachment ? 0 : 1;
            pass.attachmentReferences = {{.attachment = colorIndex}, {.attachment = depthIndex}};
            pass.attachmentDescriptions.resize(3); // Includes one untouched attachment.
            pass.subpassDescriptions.back().pDepthStencilAttachment = reinterpret_cast<vk::AttachmentReference *>(sizeof(vk::AttachmentReference));
            auto clearDepth = [&] { Check(pass.ClearDepthStencilAttachment(value, gpu), "mixed depth clear did not fold"); };
            auto clearColor = [&] { Check(pass.ClearColorAttachment(0, color, gpu), "mixed color clear did not fold"); };
            if (depthFirst) { clearDepth(); clearColor(); }
            else { clearColor(); clearDepth(); }
            Check(pass.clearValues.size() == 2, "lower attachment clear truncated higher clear value");
            Check(pass.clearValues[depthIndex].depthStencil == value, "mixed clear lost depth/stencil value");
            Check(pass.clearValues[colorIndex].color.uint32 == color.uint32, "mixed clear lost color value");
            Check(pass.attachmentDescriptions[colorIndex].loadOp == vk::AttachmentLoadOp::eClear &&
                  pass.attachmentDescriptions[colorIndex].stencilLoadOp == vk::AttachmentLoadOp::eLoad &&
                  pass.attachmentDescriptions[depthIndex].loadOp == vk::AttachmentLoadOp::eClear &&
                  pass.attachmentDescriptions[depthIndex].stencilLoadOp == vk::AttachmentLoadOp::eClear &&
                  pass.attachmentDescriptions[2].loadOp == vk::AttachmentLoadOp::eLoad &&
                  pass.attachmentDescriptions[2].stencilLoadOp == vk::AttachmentLoadOp::eLoad,
                  "mixed clear changed unrelated load operations");
            for (const auto &attachment : pass.attachmentDescriptions)
                Check(attachment.storeOp == vk::AttachmentStoreOp::eStore &&
                      attachment.stencilStoreOp == vk::AttachmentStoreOp::eStore, "mixed clear changed store operations");
        }
    }
    // Color attachments can also be cleared out of attachment-index order.
    RenderPassNode colors;
    colors.attachmentReferences = {{.attachment = 0}, {.attachment = 1}, {.attachment = 2}};
    colors.attachmentDescriptions.resize(3);
    for (u32 index : {2U, 1U, 0U})
        Check(colors.ClearColorAttachment(index, color, gpu), "reverse color clear did not fold");
    Check(colors.clearValues.size() == 3, "reverse color clear truncated higher clear values");
    for (u32 index = 0; index != 3; ++index) {
        Check(colors.clearValues[index].color.uint32 == color.uint32, "reverse color clear lost value");
        Check(colors.attachmentDescriptions[index].loadOp == vk::AttachmentLoadOp::eClear &&
              colors.attachmentDescriptions[index].stencilLoadOp == vk::AttachmentLoadOp::eLoad &&
              colors.attachmentDescriptions[index].storeOp == vk::AttachmentStoreOp::eStore &&
              colors.attachmentDescriptions[index].stencilStoreOp == vk::AttachmentStoreOp::eStore,
              "reverse color clear changed load/store operations");
    }
    std::cout << "depth/stencil clear production-method tests passed\n";
}
