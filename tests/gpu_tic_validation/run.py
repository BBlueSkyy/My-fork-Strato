#!/usr/bin/env python3
"""Execute production TIC guards and bindings with host resource facades.
No token-presence assertions. --probe-cache reproduces a known cache limitation
and intentionally exits nonzero until descriptor-dependent specialization exists.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'
HERE = pathlib.Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    paren = source.find('(', start, end)
    if paren != -1:
        depth = 1
        cursor = paren + 1
        while depth:
            depth += (source[cursor] == '(') - (source[cursor] == ')')
            cursor += 1
        end = source.index('{', cursor) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix='strato-tic-validation-') as directory:
    build = pathlib.Path(directory)
    (build / 'common').mkdir()
    (build / 'common/base.h').write_text('#pragma once\n')
    textures = (CPP / 'gpu/interconnect/common/textures.cpp').read_text()
    helper = 'static bool HasMappedTicBaseAddress('
    get_texture = function(textures, 'TextureView *Textures::GetTexture(')
    # Execute the entire pre-construction path, including cache and fallback.
    # Actual guest texture construction is covered separately by gpu_sampled_layout.
    prefix = get_texture[:get_texture.index('        auto &textureVariants{')]
    prefix += '        return &validView;\n    }\n'
    (build / 'textures.inc').write_text(function(textures, helper) + '\n' + prefix + '\n' +
        function(textures, 'Shader::TextureType Textures::GetTextureType('))
    pipeline = (CPP / 'gpu/interconnect/common/pipeline.inc').read_text()
    (build / 'bindings.inc').write_text(function(pipeline, 'union BindlessHandle') + ';\n' + '\n'.join(
        function(pipeline, signature) for signature in ('GetTextureBinding(InterconnectContext &ctx', 'GetImageBinding(InterconnectContext &ctx')))
    # Functions start with the return type on a preceding line.
    binding = (build / 'bindings.inc').read_text()
    binding = binding.replace('GetTextureBinding(InterconnectContext', 'static std::pair<vk::DescriptorImageInfo, TextureView *> GetTextureBinding(InterconnectContext')
    binding = binding.replace('GetImageBinding(InterconnectContext', 'static std::pair<vk::DescriptorImageInfo, TextureView *> GetImageBinding(InterconnectContext')
    (build / 'bindings.inc').write_text(binding)
    shader_info = ROOT / 'app/libraries/shader-compiler/shader_info.h'
    if not shader_info.exists():
        raise SystemExit('Initialize app/libraries/shader-compiler at the pinned submodule revision first')
    (build / 'shader-type.inc').write_text(function(shader_info.read_text(), 'enum class TextureType') + ';\n')
    gmmu = (CPP / 'soc/gm20b/gmmu.h').read_text()
    bits = next(line for line in gmmu.splitlines() if 'GmmuAddressSpaceBits{' in line)
    (build / 'gmmu-bits.inc').write_text(bits + '\n')
    address = (CPP / 'common/address_space.h').read_text()
    (build / 'lookup.inc').write_text(function(address, 'std::pair<span<u8>, size_t> LookupBlockLocked(') + '\n' +
        function(address, 'std::pair<span<u8>, VaType> LookupBlock('))
    graphics = (CPP / 'gpu/interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
    compute = (CPP / 'gpu/interconnect/kepler_compute/pipeline_manager.h').read_text()
    (build / 'cache.inc').write_text(function(graphics, 'Pipeline *PipelineManager::FindOrCreate(') + '\n' +
        function(graphics, 'Pipeline *Pipeline::LookupNext('))
    (build / 'compute-cache.inc').write_text(function(compute, 'Pipeline *FindOrCreate('))
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    if os.environ.get('SANITIZE', '1') != '0':
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    binary = build / 'tests'
    subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I' + str(build), '-I' + str(CPP), str(HERE / 'tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
