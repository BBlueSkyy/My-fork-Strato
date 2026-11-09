#!/usr/bin/env python3
"""Static safety guards for the temporary Fire Emblem Vulkan diagnostic."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
gpu = (ROOT / "app/src/main/cpp/skyline/gpu.cpp").read_text()
state = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/common/state_updater.h").read_text()
workflow = (ROOT / ".github/workflows/pr_build.yml").read_text()

def require(ok, message):
    if not ok:
        raise AssertionError(message)

instance = gpu.split("static vk::raii::Instance CreateInstance", 1)[1].split("static VkBool32 DebugCallback", 1)[0]
require("#ifndef NDEBUG" in instance, "Vulkan layer must be debug-only")
require('requiredLayers.push_back("VK_LAYER_KHRONOS_validation")' in instance, "Missing diagnostic validation layer")
require("VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" in instance, "Missing sync validation")
require(".pNext = instancePNext" in instance, "Validation configuration not passed to Vulkan instance")
require("synchronization=true (DevDebug)" in instance, "Missing layer confirmation in logs")

require("FE3H-DESC BEFORE" in state and "FE3H-DESC AFTER" in state, "Missing driver-call boundary logs")
require("std::atomic<u64> descriptorUpdateSequence" in state, "Descriptor log must be rate-limited")
require("descriptorSeq <= 256 || (descriptorSeq % 32) == 0" in state, "Diagnostic log cap missing")
probe = state.split("FE3H-DESC BEFORE")[0].rsplit("#ifndef NDEBUG", 1)[-1]
require("descriptorUpdateSequence" in probe, "Descriptor probe must remain behind debug guard")
require("gpu.vkDevice.updateDescriptorSets(updateInfo->writes, {})" in state, "Release descriptor update removed")

require("assembleDevRelease" not in workflow, "Must compile only DevDebug")
require("arm64-release.apk" not in workflow, "Release artifact must not be uploaded")
layer = workflow.index("Install Khronos Validation Layer 1.4.363.1")
debug = workflow.index("assembleDevDebug")
require(layer < debug, "Validation layer must be installed before compiling DevDebug")
require("app/src/debug/jniLibs/arm64-v8a/libVkLayer_khronos_validation.so" in workflow,
        "Validation library must be installed in the Debug jniLibs source set")
require("2c17b00a95b18fd63244b27332ef86f45c9269769f798a6261ce80b869c41af4" in workflow,
        "Unpinned validation layer download")
require("lib/arm64-v8a/libVkLayer_khronos_validation.so" in workflow, "Debug APK must verify layer packaging")
require("strato-pr${{ github.event.number }}-arm64-debug-validation.apk" in workflow, "Missing Debug artifact")
print("Fire Emblem descriptor probe guards: PASS")
