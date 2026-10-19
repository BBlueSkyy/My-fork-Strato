#!/usr/bin/env python3
"""Exercise production TraitManager negotiation and ShaderManager profile wiring."""
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
with tempfile.TemporaryDirectory(prefix='strato-shader-features-') as tmp:
    tmp = Path(tmp)
    (tmp / 'shader_compiler').symlink_to(SHADER.resolve(), target_is_directory=True)
    header = (GPU / 'trait_manager.h').read_text()
    (tmp / 'traits.inc').write_text(header[header.index('namespace skyline'):])
    source = (GPU / 'trait_manager.cpp').read_text()
    baseline = os.environ.get('TRAITS_BASELINE_REF')
    if baseline:
        source = subprocess.check_output(['git', 'show', baseline + ':app/src/main/cpp/skyline/gpu/trait_manager.cpp'], cwd=ROOT, text=True)
    (tmp / 'constructor.inc').write_text('namespace skyline::gpu {\n' + function(source, 'TraitManager::TraitManager(') + '\n}')
    manager = (GPU / 'shader_manager.cpp').read_text()
    profile = function(manager, 'profile = Shader::Profile{') + ';'
    (tmp / 'profile.inc').write_text(profile)
    gpu = (ROOT / 'app/src/main/cpp/skyline/gpu.cpp').read_text()
    start = gpu.index('auto deviceFeatures2{physicalDevice.getFeatures2<')
    end = gpu.index('};', start) + 2
    (tmp / 'query.inc').write_text(gpu[start:end])
    binding = (GPU / 'interconnect/common/pipeline.inc').read_text()
    start = binding.index('        size_t padding{ssbo.address')
    end = binding.index('        if (!range)', start)
    (tmp / 'range.inc').write_text(binding[start:end])
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-unused-variable', '-fsanitize=undefined', '-fno-sanitize-recover=all']
    include = [os.environ.get('VULKAN_HPP_INCLUDE', str(ROOT / 'app/libraries/vkhpp')),
               os.environ.get('VULKAN_HEADERS_INCLUDE', str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))]
    subprocess.run(['c++', *flags, '-I' + str(tmp), *['-I' + p for p in include], str(HERE / 'tests.cpp'), '-o', str(tmp / 'tests')], check=True)
    subprocess.run([str(tmp / 'tests')], check=True)
