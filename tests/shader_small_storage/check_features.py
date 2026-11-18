#!/usr/bin/env python3
"""Guard the host/profile interface and diagnostic APK isolation."""
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
assert '#ifndef NDEBUG' in gpu and 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT' in gpu
assert 'MK11-' not in gpu and 'MK11-' not in traits
workflow=(root/'.github/workflows/pr_build.yml').read_text()
# This PR is a Release A/B build of the same CAS code. Retain the strict
# validation APK requirements for the DevDebug workflow, but explicitly verify
# that the DevRelease workflow builds WITHOUT packaging or installing VVL.
if 'assembleDevDebug' in workflow:
    assert 'assembleDevRelease' not in workflow
    assert 'android-binaries-1.4.363.1.zip' in workflow
    assert '2c17b00a95b18fd63244b27332ef86f45c9269769f798a6261ce80b869c41af4' in workflow
    assert 'lib/arm64-v8a/libVkLayer_khronos_validation.so' in workflow
    print('PASS: enabled-feature wiring, Vulkan 1.1, SSBO alignment and pinned DevDebug VVL APK')
elif 'assembleDevRelease' in workflow:
    assert 'assembleDevDebug' not in workflow
    assert 'Install Khronos Validation Layer' not in workflow
    assert 'android-binaries-1.4.363.1.zip' not in workflow
    assert 'app/build/outputs/apk/dev/release/app-dev-release.apk' in workflow
    assert 'assert validation not in names' in workflow
    assert 'lib/arm64-v8a/libVkLayer_khronos_validation.so' in workflow
    assert 'Upload ARM64 DevRelease APK' in workflow
    print('PASS: enabled-feature wiring, Vulkan 1.1, SSBO alignment and DevRelease APK without VVL')
else:
    raise AssertionError('PR Build must assemble exactly one supported Dev APK variant')
