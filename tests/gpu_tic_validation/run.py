#!/usr/bin/env python3
"""Execute production TIC guards and bindings with host resource facades.
No token-presence assertions. --probe-cache exercises specialized pipeline reuse
and dirty-state/descriptor paths; --vulkan-lifetime also executes real pending work.
"""
import os
import pathlib
import subprocess
import sys
import shutil
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
    # Exercise the production bindless CBUF read, including unbound secondary
    # handling and range validation, with the same host CBUF fixture.
    (build / 'bindless-read.inc').write_text(
        'static BindlessHandle ' + function(pipeline, 'ReadBindlessHandle(InterconnectContext &ctx,') + '\n' +
        function(pipeline, 'static void SnapshotBindlessHandles(') + '\n')
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
    (build / 'shader-type.inc').write_text(function(shader_info.read_text(), 'enum class TextureType') + ';\n' + function((ROOT/'app/libraries/shader-compiler/environment.h').read_text(),'struct TextureHandleSource') + ';\n' + function((ROOT/'app/libraries/shader-compiler/environment.h').read_text(),'enum class TextureQueryKind') + ';\n')
    gmmu = (CPP / 'soc/gm20b/gmmu.h').read_text()
    bits = next(line for line in gmmu.splitlines() if 'GmmuAddressSpaceBits{' in line)
    (build / 'gmmu-bits.inc').write_text(bits + '\n')
    address = (CPP / 'common/address_space.h').read_text()
    (build / 'lookup.inc').write_text(function(address, 'std::pair<span<u8>, size_t> LookupBlockLocked(') + '\n' +
        function(address, 'std::pair<span<u8>, VaType> LookupBlock('))
    common = (CPP / 'gpu/interconnect/common/common.h').read_text()
    (build / 'constant-read.inc').write_text(function(common, 'template<typename T>\n        T ReadForShaderCompile('))
    graphics = (CPP / 'gpu/interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
    compute = (CPP / 'gpu/interconnect/kepler_compute/pipeline_manager.h').read_text()
    (build / 'graphics-query.inc').write_text(function(graphics, '[&](const Shader::TextureHandleSource &source, Shader::TextureQueryKind query) -> u32'))
    (build / 'scalar-read.inc').write_text(function(graphics, '[&](u32 index, u32 offset)'))
    (build / 'cache.inc').write_text(function(graphics, 'Pipeline *PipelineManager::FindOrCreate(') + '\n' +
        function(graphics, 'Pipeline *Pipeline::LookupNext(') + '\n' + function(graphics, 'bool Pipeline::MatchesSpecialization(') + '\n' + function(graphics, 'bool Pipeline::CheckBindingMatch('))
    compute_source = (CPP / 'gpu/interconnect/kepler_compute/pipeline_manager.cpp').read_text()
    # Execute the real snapshot passes too: ordering must precede callbacks
    # that mark this draw's storage outputs dirty, across all shader stages.
    for name, source, signature in (
        ('graphics-handles', graphics, 'DescriptorUpdateInfo *Pipeline::SyncDescriptors('),
        ('quick-handles', graphics, 'DescriptorUpdateInfo *Pipeline::SyncDescriptorsQuickBind('),
        ('compute-handles', compute_source, 'DescriptorUpdateInfo *Pipeline::SyncDescriptors('),
    ):
        body = function(source, signature)
        start = body.index('        auto handles{')
        end = body.index('        handleIt = handles.data();', start) + len('        handleIt = handles.data();')
        (build / (name + '.inc')).write_text(body[start:end] + '\n')
    (build / 'compute-cache.inc').write_text(function(compute, 'Pipeline *FindOrCreate('))
    (build / 'compute-match.inc').write_text(function(compute_source, 'bool Pipeline::MatchesSpecialization('))
    prepare = (CPP / 'gpu/interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    (build / 'prepare-descriptors.inc').write_text(function(prepare, '[&]() -> DescriptorUpdateInfo *'))
    state = (CPP / 'gpu/interconnect/maxwell_3d/pipeline_state.cpp').read_text()
    (build / 'refresh.inc').write_text(function(state, 'bool PipelineState::Refresh('))
    dirty = (CPP / 'common/dirty_tracking.h').read_text()
    (build / 'dirty.inc').write_text('namespace dirty {\n' + '\n'.join(function(dirty, signature) + ';' for signature in (
        'class ManualDirty ', 'class RefreshableManualDirty ', 'class CachedManualDirty ', 'template<typename T> requires (std::is_base_of_v<ManualDirty, T>)')) + '\n}\n')
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    if os.environ.get('SANITIZE', '1') != '0':
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    arguments = list(sys.argv[1:])
    libraries = []
    if '--vulkan-lifetime' in arguments:
        arguments.remove('--vulkan-lifetime')
        source = build / 'lifetime.spvasm'
        source.write_text('OpCapability Shader\nOpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main "main"\nOpExecutionMode %main LocalSize 1 1 1\n%void = OpTypeVoid\n%fn = OpTypeFunction %void\n%main = OpFunction %void None %fn\n%entry = OpLabel\nOpReturn\nOpFunctionEnd\n')
        module = source.with_suffix('.spv')
        subprocess.run([shutil.which('spirv-as') or 'spirv-as','--target-env','vulkan1.2',str(source),'-o',str(module)],check=True)
        subprocess.run([shutil.which('spirv-val') or 'spirv-val','--target-env','vulkan1.2',str(module)],check=True)
        arguments += [str(module)]
        flags += ['-DTIC_VULKAN_LIFETIME','-Wno-missing-field-initializers','-I'+os.environ.get('VULKAN_HEADERS_INCLUDE_DIR','/usr/include')]
        libraries = [os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan')]
    binary = build / 'tests'
    subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I' + str(build), '-I' + str(CPP), str(HERE / 'tests.cpp'), *libraries, '-o', str(binary)], check=True)
    spec_binary = build / 'specialization'
    subprocess.run([os.environ.get('CXX', 'g++'), *flags, '-I' + str(CPP), str(HERE / 'specialization.cpp'), '-o', str(spec_binary)], check=True)
    subprocess.run([str(spec_binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    subprocess.run([str(binary), *arguments], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
