#!/usr/bin/env python3
"""Exercise the production snapshot/update policy with UBSan."""
from pathlib import Path
import subprocess
import shutil
import re
import os
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/common/dynamic_descriptors.cpp').read_text()
begin = source.index('namespace skyline::gpu::interconnect::dynamic_descriptor_detail {')
end = source.index('// End independently testable descriptor policy.', begin)
with tempfile.TemporaryDirectory(prefix='strato-dynamic-descriptors-') as directory:
    path = Path(directory)
    (path / 'policy.inc').write_text(source[begin:end])
    executable = path / 'tests'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I' + str(path), str(Path(__file__).with_name('tests.cpp')), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    # Compile exact runtime bodies with real pinned Vulkan RAII/handle APIs.
    header = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/common/dynamic_descriptors.h').read_text()
    (path / 'header.inc').write_text(header[header.index('namespace skyline'):])
    (path / 'source.inc').write_text(source[begin:])
    runtime = path / 'runtime'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I' + str(path), str(Path(__file__).with_name('runtime.cpp')), '-o', str(runtime)], check=True)
    subprocess.run([str(runtime)], check=True)
    # Derive collaborator-owned API signatures from real headers to catch drift.
    common = ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/common'
    for filename, methods in [('textures', ['GetPoolHeaders', 'GetDynamicTexture', 'GetDynamicTextureComponentType', 'DynamicTextureSupportsLinear']), ('samplers', ['GetPoolHeaders', 'IsViaHeaderBinding', 'GetDynamicSampler', 'DynamicSamplerUsesLinear'])]:
        text = (common / (filename + '.h')).read_text()
        signatures = []
        for method in methods:
            match = re.search(r'[^\n;{}]+\b' + method + r'\([^;{}]*\)(?: const)?', text)
            if match is None:
                raise RuntimeError('Missing actual pool API ' + method)
            signatures.append(match.group(0).strip() + ';')
        (path / (filename + '_api.inc')).write_text('\n'.join(signatures))
    shader = Path(os.environ.get('SHADER_COMPILER_ROOT', ROOT.parent / 'shader-compiler'))
    info = (shader / 'shader_info.h').read_text()
    dynamic_begin = info.index('struct DynamicTextureDescriptor {')
    dynamic_end = info.index('{', dynamic_begin) + 1
    depth = 1
    while depth:
        depth += (info[dynamic_end] == '{') - (info[dynamic_end] == '}')
        dynamic_end += 1
    (path / 'dynamic_shader.inc').write_text(info[dynamic_begin:dynamic_end] + ';')
    subprocess.run([shutil.which('clang++') or 'c++', '-std=c++20', '-fsyntax-only', '-Werror', '-Wno-unused-function', '-Wno-narrowing',
        '-DVULKAN_HPP_NO_SPACESHIP_OPERATOR', '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-DVULKAN_HPP_NO_SETTERS',
        '-DVULKAN_HPP_NO_SMART_HANDLE', '-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1', '-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0',
        '-I' + str(path), '-I' + str(ROOT / 'app/libraries/vkhpp'),
        '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
        str(Path(__file__).with_name('native_compile.cpp'))], check=True)
    print('PASS exact dynamic descriptor runtime native Vulkan-Hpp compile')
