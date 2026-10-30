#!/usr/bin/env python3
"""Compile exact Vulkan creation code and test swizzles with pinned Vulkan-Hpp.

Requires the vkhpp (including Vulkan-Headers) and vkma submodules. This is a
host compilation check, not device execution or a replacement for APK CI.
"""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/common/textures.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='strato-sparse-vulkan-') as tmp:
    tmp = pathlib.Path(tmp)
    for name, start, end in [
        ('sparse-vulkan.inc', '    static std::shared_ptr<Texture> CreateSparseZeroImage(', '    TextureView *Textures::GetSparseTexture('),
        ('sparse-swizzle.inc', '    static vk::ComponentMapping ConvertSparseZeroSwizzle(', '    static std::shared_ptr<Texture> CreateSparseZeroImage('),
    ]:
        begin = source.index(start)
        (tmp / name).write_text(source[begin:source.index(end, begin)])
    command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-Wall', '-Wextra',
               '-Wno-unused-function', '-Wno-missing-field-initializers',
               '-I' + str(tmp), '-I' + str(ROOT / 'app/libraries/vkhpp'),
               '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
               '-I' + str(ROOT / 'app/libraries/vkma/include'),
               str(ROOT / 'tests/gpu_sparse/vulkan.cpp')]
    subprocess.run([*command, '-fsyntax-only'], check=True)
    print('PASS: sparse initialization compiles against pinned Vulkan-Hpp/VMA', flush=True)
    executable = tmp / 'swizzle-tests'
    subprocess.run([*command, '-DSWIZZLE_ONLY', '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
