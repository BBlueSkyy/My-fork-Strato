#!/usr/bin/env python3
"""Fail CI if MK11 Vulkan diagnostics escape DevDebug or lose the pinned layer."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
gpu = (root / "app/src/main/cpp/skyline/gpu.cpp").read_text()
allocator = (root / "app/src/main/cpp/skyline/gpu/descriptor_allocator.cpp").read_text()
workflow = (root / ".github/workflows/pr_build.yml").read_text()

instance = gpu.split("static vk::raii::Instance CreateInstance(", 1)[1].split(
    "static VkBool32 DebugCallback(", 1
)[0]
assert '#ifndef NDEBUG' in instance, "Validation layers must be Debug-only"
assert 'requiredLayers.push_back("VK_LAYER_KHRONOS_validation")' in instance
assert "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" in instance
assert ".pNext = instancePNext" in instance
assert "Vulkan validation enabled:" in instance

assert "MK11-DESC pool-created" in allocator
assert "MK11-DESC allocation-failed" in allocator
assert allocator.count("#ifndef NDEBUG") >= 2, "Pool diagnostics must be Debug-only"
assert "vk::Result::eErrorOutOfPoolMemory" in allocator
assert "vk::Result::eErrorFragmentedPool" in allocator

assert "android-binaries-1.4.363.1.zip" in workflow
assert "2c17b00a95b18fd63244b27332ef86f45c9269769f798a6261ce80b869c41af4" in workflow
assert "app/src/debug/jniLibs/arm64-v8a/libVkLayer_khronos_validation.so" in workflow
assert "assembleDevDebug" in workflow
assert "assembleDevRelease" not in workflow
assert "lib/arm64-v8a/libVkLayer_khronos_validation.so" in workflow
assert "arm64-debug-validation.apk" in workflow

print("MK11 diagnostic guards: OK (DevDebug only, pinned validation layer, bounded pool probes)")
