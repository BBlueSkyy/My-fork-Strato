// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors
#pragma once

#include <vulkan/vulkan.h>

/*
 * The Android/NDK Vulkan headers bundled by older Strato toolchains predate
 * VK_EXT_attachment_feedback_loop_layout even when the runtime driver exposes it.
 *
 * Keep the compatibility surface minimal and numerically identical to the Vulkan
 * specification so feature discovery/device creation can use the extension
 * without requiring a wholesale Vulkan-Hpp update.
 */
#ifndef VK_EXT_attachment_feedback_loop_layout
#define VK_EXT_attachment_feedback_loop_layout 1
#define VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_SPEC_VERSION 2
#define VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME "VK_EXT_attachment_feedback_loop_layout"

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT ((VkStructureType)1000339000)
#define VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT ((VkImageLayout)1000339000)
#define VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT ((VkImageUsageFlagBits)0x00080000)
#define VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT ((VkPipelineCreateFlagBits)0x02000000)
#define VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT ((VkPipelineCreateFlagBits)0x04000000)
#define VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT ((VkDependencyFlagBits)0x00000008)

typedef struct VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT {
    VkStructureType sType;
    void *pNext;
    VkBool32 attachmentFeedbackLoopLayout;
} VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT;
#endif
