#!/usr/bin/env python3
"""Guard storage-feature wiring and production APK isolation."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
gpu = (root / 'app/src/main/cpp/skyline/gpu.cpp').read_text()
traits = (root / 'app/src/main/cpp/skyline/gpu/trait_manager.cpp').read_text()
header = (root / 'app/src/main/cpp/skyline/gpu/trait_manager.h').read_text()
manager = (root / 'app/src/main/cpp/skyline/gpu/shader_manager.cpp').read_text()
assert 'VkApiVersion{VK_API_VERSION_1_1}' in (root/'app/src/main/cpp/skyline/gpu.h').read_text()
for bits in (8,16):
    structure = f'vk::PhysicalDevice{bits}BitStorageFeatures'
    assert structure in gpu and structure in header
    for feature, prop in [(f'storageBuffer{bits}BitAccess',f'supportsStorageBuffer{bits}BitAccess'),
                          (f'uniformAndStorageBuffer{bits}BitAccess',f'supportsUniformAndStorageBuffer{bits}BitAccess')]:
        assert f'FEAT_SET({structure}, {feature}, {prop})' in traits
    assert f'.support_storage_buffer_{bits}bit = traits.supportsStorageBuffer{bits}BitAccess' in manager
    assert f'.support_uniform_buffer_{bits}bit = traits.supportsUniformAndStorageBuffer{bits}BitAccess && traits.supportsUniformBufferStandardLayout' in manager
assert 'EXT_SET("VK_KHR_8bit_storage", has8BitStorageExt)' in traits
assert re.search(r'if \(has8BitStorageExt\).*?FEAT_SET\(vk::PhysicalDevice8BitStorageFeatures.*?else.*?unlink<vk::PhysicalDevice8BitStorageFeatures>',traits,re.S)
assert 'minimumStorageBufferAlignment = std::max<u32>(sizeof(u32),' in traits
assert 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT' not in gpu
assert 'VK_LAYER_KHRONOS_validation' not in gpu
assert '.enabledLayerCount = 0' in gpu and '.ppEnabledLayerNames = nullptr' in gpu
assert 'MK11-' not in gpu and 'MK11-' not in traits
workflow=(root/'.github/workflows/pr_build.yml').read_text()
assert 'assembleDevRelease' in workflow and 'assembleDevDebug' not in workflow
assert 'Install Khronos Validation Layer' not in workflow
assert 'android-binaries-1.4.363.1.zip' not in workflow
assert 'Test 8/16-bit storage backend (ASan/UBSan and SPIR-V Tools)' in workflow
assert 'app-dev-release.apk' in workflow
assert 'assert apk.testzip() is None' in workflow
assert 'apksigner' in workflow
assert 'assert "lib/arm64-v8a/libVkLayer_khronos_validation.so" not in libraries' in workflow
print('PASS: feature/profile wiring, Vulkan 1.1, aligned SSBO, CAS tests and clean signed DevRelease without VVL')
