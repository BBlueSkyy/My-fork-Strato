#!/usr/bin/env python3
"""Compile the actual TraitManager negotiation and ShaderManager profile gates."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
HERE = Path(__file__).resolve().parent
SHADER = Path(os.environ.get('SHADER_COMPILER_ROOT', ROOT / 'app/libraries/shader-compiler'))
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
with tempfile.TemporaryDirectory(prefix='strato-dynamic-features-') as tmp:
    tmp = Path(tmp)
    (tmp / 'shader_compiler').symlink_to(SHADER.resolve(), target_is_directory=True)
    header = (GPU / 'trait_manager.h').read_text()
    (tmp / 'traits.inc').write_text(header[header.index('namespace skyline'):])
    source = (GPU / 'trait_manager.cpp').read_text()
    (tmp / 'constructor.inc').write_text('namespace skyline::gpu {\n' + function(source, 'TraitManager::TraitManager(') + '\n}')
    manager = (GPU / 'shader_manager.cpp').read_text()
    (tmp / 'profile.inc').write_text(function(manager, 'profile = Shader::Profile{') + ';')
    compute = manager[manager.index('    class ComputeEnvironment :'):]
    graphics = manager[manager.index('    class GraphicsEnvironment :'):manager.index('    class ComputeEnvironment :')]
    (tmp / 'graphics_gate.inc').write_text(function(manager, '        bool GraphicsDynamicTextureHandlesSupported('))
    graphics_method = function(graphics, '        [[nodiscard]] bool SupportsDynamicTextureHandles()').replace(' final', '')
    (tmp / 'graphics_environment.inc').write_text(graphics_method)
    assert 'getTextureCompareFunc, traceTarget, gpu.traits.supportsDynamicTextureHandles};' in manager
    method = function(compute, '        [[nodiscard]] bool SupportsDynamicTextureHandles()')
    method = method.replace(' final', '')
    (tmp / 'environment.inc').write_text(method)
    gpu = (ROOT / 'app/src/main/cpp/skyline/gpu.cpp').read_text()
    start = gpu.index('auto deviceFeatures2{physicalDevice.getFeatures2<')
    (tmp / 'query.inc').write_text(gpu[start:gpu.index('};', start) + 2])
    assert 'deviceProperties2, physicalDevice, VkApiVersion};' in gpu
    # Share only the fixture declarations; the tests below exercise production
    # constructor/profile source, not a mirrored implementation of the gate.
    harness = (ROOT / 'tests/gpu_shader_features/tests.cpp').read_text().split('int main() {')[0]
    (tmp / 'tests.cpp').write_text(harness + (HERE / 'tests.cpp').read_text())
    binding = (GPU / 'interconnect/common/pipeline.inc').read_text()
    start = binding.index('        size_t padding{ssbo.address')
    (tmp / 'range.inc').write_text(binding[start:binding.index('        if (!range)', start)])
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-unused-variable', '-fsanitize=undefined', '-fno-sanitize-recover=all']
    include = [os.environ.get('VULKAN_HPP_INCLUDE', str(ROOT / 'app/libraries/vkhpp')),
               os.environ.get('VULKAN_HEADERS_INCLUDE', str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))]
    subprocess.run(['c++', *flags, '-I' + str(tmp), *['-I' + p for p in include], str(tmp / 'tests.cpp'), '-o', str(tmp / 'tests')], check=True)
    subprocess.run([str(tmp / 'tests')], check=True)
