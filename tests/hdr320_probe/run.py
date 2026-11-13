#!/usr/bin/env python3
"""Compile production observation bodies/format strings against host state facades.

No Vulkan calls: verifies snapshot retention and cache-only reads, not device LOD.
"""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
DEPS = pathlib.Path(os.environ.get('STRATO_TEST_DEPS', ROOT / 'app/libraries'))

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='hdr320-probe-') as directory:
    build = pathlib.Path(directory)
    sampler = (GPU / 'interconnect/common/samplers.cpp').read_text()
    textures = (GPU / 'interconnect/common/textures.cpp').read_text()
    pipeline = (GPU / 'interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
    draw = (GPU / 'interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    span_source = (GPU.parent / 'common/span.h').read_text()
    span_type = function(span_source, 'template<typename T, size_t Extent = std::dynamic_extent>')
    (build / 'span.inc').write_text(span_type + ';\n')
    texture_header = (GPU / 'texture/texture.h').read_text()
    (build / 'getter.inc').write_text(function(texture_header, 'u64 GetBackingGeneration()'))
    begin = draw.index('         span<diagnostic::HdrSampleBinding> observedBindings{hdrBindings};')
    end = draw.index('         auto *descUpdateInfo', begin)
    (build / 'span_cap.inc').write_text(draw[begin:end])
    # This observer accesses channelSequenceNumber, requiring the definition in
    # the actual translation unit (the host context facade cannot prove includes).
    assert '#include <soc/gm20b/channel.h>' in pipeline
    (build / 'observation.inc').write_text('\n'.join([
        function(textures, 'bool Textures::ObserveBoundTic('),
        function(sampler, 'u32 Samplers::ObserveBoundSamplerIndex('),
        function(sampler, 'bool Samplers::ObserveBoundSampler('),
        function(pipeline, 'static void ObserveHdrBinding(')]))
    start = draw.index('        if (const auto *pipeline{activeState.GetPipeline()}; pipeline && diagnostic::HdrDrawSelected(')
    end = draw.index('\n        constantBuffers.ResetQuickBind();', start)
    (build / 'draw.inc').write_text(draw[start:end])
    cmd = [os.environ.get('CXX', 'c++'), '-std=c++20', '-DFMT_HEADER_ONLY', '-DFMT_CONSTEVAL=', '-Wall', '-Wextra',
           '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-copy', '-Wno-defaulted-function-deleted', '-fsanitize=undefined', '-fno-sanitize-recover=all',
           '-I'+str(build), '-I'+str(GPU),
           # Match app/CMakeLists.txt's fmt mode; keep third-party deprecations
           # separate from -Werror on the production observer bodies under test.
           '-isystem', str(DEPS / 'fmt/include'),
           '-I'+str(pathlib.Path(os.environ.get('VULKAN_HPP_INCLUDE_DIR', DEPS / 'vkhpp'))),
           '-I'+str(pathlib.Path(os.environ.get('VULKAN_HEADERS_INCLUDE_DIR', DEPS / 'vkhpp/Vulkan-Headers/include'))),
           str(pathlib.Path(__file__).with_name('tests.cpp')), '-o', str(build / 'tests')]
    subprocess.run(cmd, check=True)
    subprocess.run([str(build / 'tests')], check=True)
