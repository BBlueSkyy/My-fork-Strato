#!/usr/bin/env python3
"""Compile and run exact integration bodies with controlled host collaborators."""
from pathlib import Path
import subprocess
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'app/src/main/cpp/skyline/gpu/interconnect'
HERE = Path(__file__).resolve().parent

def function(text, mark):
    start = text.index(mark)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

with tempfile.TemporaryDirectory(prefix='strato-dynamic-integration-') as directory:
    path = Path(directory)
    pool = (BASE / 'common/dynamic_pool.h').read_text()
    (path / 'pool.inc').write_text(pool[pool.index('namespace skyline'):])
    intervals = (ROOT / 'app/src/main/cpp/skyline/common/interval_list.h').read_text()
    (path / 'intervals.inc').write_text(intervals[intervals.index('namespace skyline'):])
    updater = (BASE / 'common/state_updater.h').read_text()
    (path / 'bind.inc').write_text(function(updater, '    struct BindCachedDescriptorSetCmdImpl') + ';')
    (path / 'append.inc').write_text(function(updater, '        void BindCachedComputeDescriptors('))
    pipeline = (BASE / 'kepler_compute/pipeline_manager.cpp').read_text()
    layout = function(pipeline, '    static Pipeline::CompiledPipeline MakeCompiledPipeline(')
    (path / 'layout.inc').write_text(layout)
    header = (BASE / 'kepler_compute/pipeline_manager.h').read_text()
    (path / 'variant.inc').write_text(function(header, '        Pipeline *FindOrCreate('))
    dispatch = (BASE / 'kepler_compute/kepler_compute.cpp').read_text()
    (path / 'dispatch.inc').write_text(function(dispatch, '    void KeplerCompute::Dispatch('))
    (path / 'dispatch_callback.inc').write_text('auto callback = ' + function(dispatch, '[drawParams](vk::raii::CommandBuffer &commandBuffer') + ';')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-missing-field-initializers', '-fsanitize=undefined', '-fno-sanitize-recover=all']
    subprocess.run(['c++', *flags, '-I' + str(path), str(HERE / 'tests.cpp'), '-o', str(path / 'tests')], check=True)
    subprocess.run([str(path / 'tests')], check=True)
    clang = shutil.which('clang++')
    native_layout = layout
    if not clang:
        # GCC requires declaration order; change only initializer spelling.
        native_layout = native_layout.replace('.pBindings = layoutBindings.data(),\n            .bindingCount = static_cast<u32>(layoutBindings.size()),', '.bindingCount = static_cast<u32>(layoutBindings.size()),\n            .pBindings = layoutBindings.data(),')
        native_layout = native_layout.replace('.pSetLayouts = layouts.data(),\n            .setLayoutCount = dynamicDescriptors ? 2U : 1U,', '.setLayoutCount = dynamicDescriptors ? 2U : 1U,\n            .pSetLayouts = layouts.data(),')
        native_layout = native_layout.replace('.pipeline = std::move(pipeline),\n            .pipelineLayout = std::move(pipelineLayout),\n            .descriptorSetLayout = std::move(descriptorSetLayout),', '.descriptorSetLayout = std::move(descriptorSetLayout),\n            .pipelineLayout = std::move(pipelineLayout),\n            .pipeline = std::move(pipeline),')
    (path / 'native_layout.inc').write_text(native_layout)
    subprocess.run([clang or 'c++', '-std=c++20', '-fsyntax-only', '-Werror', '-Wno-unused-function', *(['-Wno-reorder-init-list'] if clang else []),
        '-DVULKAN_HPP_NO_SPACESHIP_OPERATOR', '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-DVULKAN_HPP_NO_SETTERS',
        '-DVULKAN_HPP_NO_SMART_HANDLE', '-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1', '-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0',
        '-I' + str(path), '-I' + str(ROOT / 'app/libraries/vkhpp'),
        '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
        str(HERE / 'native_compile.cpp')], check=True)
    print('PASS cached-set recording and compute pipeline layout native Vulkan-Hpp compile')
