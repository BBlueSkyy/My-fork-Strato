#!/usr/bin/env python3
"""Compile production device creation/reporting and test actual feature negotiation with pinned Vulkan-Hpp.

The trait constructor is stopped before unrelated physical-device format/memory
queries. Logging and Android types are host stubs; Vulkan declarations are real.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
HPP = Path(os.environ.get('VULKAN_HPP', ROOT / 'app/libraries/vkhpp'))


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix='strato-fault-vulkan-') as directory:
    directory = Path(directory)
    header = (GPU / 'trait_manager.h').read_text()
    (directory / 'traits.inc').write_text(header[header.index('namespace skyline'):])
    source = (GPU / 'trait_manager.cpp').read_text()
    constructor = source[source.index('    TraitManager::TraitManager('):source.index('        #undef FEAT_SET')]
    (directory / 'negotiate.inc').write_text(constructor + '\n#undef FEAT_SET\n}\n')
    source = (GPU.parent / 'gpu.cpp').read_text()
    (directory / 'create_device.inc').write_text(function(source, '    static vk::raii::Device CreateDevice('))
    (directory / 'report.inc').write_text(function(source, '    void GPU::ReportDeviceLoss('))
    (directory / 'wait.inc').write_text(function((GPU / 'fence_cycle.h').read_text(), '        void Wait('))
    binary = directory / 'tests'
    subprocess.run(['g++', '-std=c++20', '-Werror', '-Wno-unused-function', '-DFMT_HEADER_ONLY',
                    '-DVULKAN_HPP_NO_SPACESHIP_OPERATOR', '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS',
                    '-DVULKAN_HPP_NO_SETTERS', '-DVULKAN_HPP_NO_SMART_HANDLE',
                    '-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1', '-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0',
                    '-I' + str(directory), '-I' + str(HPP), '-I' + str(HPP / 'Vulkan-Headers/include'),
                    '-I' + str(ROOT / 'app/libraries/fmt/include'), '-I' + str(GPU.parent),
                    str(Path(__file__).with_name('vulkan.cpp')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS pinned Vulkan-Hpp device creation/reporting compile and passthrough/fault feature negotiation')
