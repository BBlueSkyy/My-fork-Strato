#!/usr/bin/env python3
"""Exercise exact production TIC conversion, numeric queries and bundle persistence."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
def namespace(text):
    return text[text.index('namespace skyline'):]
with tempfile.TemporaryDirectory(prefix='strato-texture-components-') as tmp:
    tmp = Path(tmp)
    shader = Path(os.environ.get('SHADER_COMPILER_ROOT', ROOT / 'app/libraries/shader-compiler'))
    info = (shader / 'shader_info.h').read_text() + (shader / 'runtime_info.h').read_text()
    (tmp / 'shader.inc').write_text('namespace Shader {\n' + '\n'.join(function(info, 'enum class ' + name) + ';' for name in ('TextureType', 'TextureComponentType', 'CompareFunction')) + '\n}')
    header = (GPU / 'texture/texture.h').read_text()
    start = header.index('struct FormatBase')
    end = header.index('constexpr bool IsCompressed()', start)
    # Keep the complete real metadata, its aspect selector, equality and wrapper.
    metadata = header[start:end] + function(header, 'constexpr vk::ImageAspectFlags Aspect(') + '\n' + function(header, 'constexpr bool operator==(const FormatBase &') + '\n};\n' + function(header, 'class Format {') + ';'
    table = namespace((GPU / 'texture/format.h').read_text())
    table = table.replace('bitsPerBlock / 8, vk::Format::format,', '.bpb = bitsPerBlock / 8, .vkFormat = vk::Format::format,')
    table = table.replace(', vka::eDepth,', ', .vkAspect = vka::eDepth,')
    # GCC needs declaration-order designators; keep every metadata value.
    import re
    table = re.sub(r'\.blockWidth = ([0-9]+),\s*\.blockHeight = ([0-9]+)', r'.blockHeight = \2, .blockWidth = \1', table)
    table = table.replace('}, true);', '}, .stencilFirst = true);')
    (tmp / 'format.inc').write_text('namespace skyline::gpu::texture {\n' + metadata + '\n}\n' + table)
    (tmp / 'tic.inc').write_text(namespace((GPU / 'interconnect/common/tic.h').read_text()))
    textures = (GPU / 'interconnect/common/textures.cpp').read_text()
    baseline = os.environ.get('TEXTURE_COMPONENT_NEGATIVE_CONTROL')
    if baseline == 'collapse-sint':
        textures = textures.replace('return Shader::TextureComponentType::Sint;', 'return Shader::TextureComponentType::Uint;')
    elif baseline == 'ignore-stencil':
        textures = textures.replace('if (format->Aspect(firstComponent) == vk::ImageAspectFlagBits::eStencil)', 'if (false)')
    (tmp / 'components.inc').write_text('\n'.join(function(textures, sig) for sig in ('static texture::Format ConvertTicFormat(', 'static Shader::TextureComponentType GetTicComponentType(', 'Shader::TextureComponentType Textures::GetTextureComponentType(')))
    (tmp / 'dynamic.inc').write_text('\n'.join(function(textures, sig) for sig in ('TextureView *Textures::GetDynamicTexture(', 'bool Textures::DynamicTextureSupportsLinear(', 'Shader::TextureComponentType Textures::GetDynamicTextureComponentType(')))
    mapping_start = textures.index('            if (auto tracked{dynamicTextureMappings.find(textureHeader)};')
    mapping_end = textures.index('            texture = ctx.gpu.texture.FindOrCreate', mapping_start)
    (tmp / 'mapping.inc').write_text(textures[mapping_start:mapping_end])
    bundle_header = namespace((GPU / 'interconnect/common/pipeline_state_bundle.h').read_text())
    bundle_source = namespace((GPU / 'interconnect/common/pipeline_state_bundle.cpp').read_text())
    if baseline == 'collapse-bundle':
        bundle_source = bundle_source.replace('static_cast<u32>(componentType)', 'static_cast<u32>(componentType != Shader::TextureComponentType::Float)')
    (tmp / 'bundle.inc').write_text(bundle_header + '\n' + bundle_source)
    manager = (GPU / 'shader_manager.cpp').read_text()
    manager_header = (GPU / 'shader_manager.h').read_text()
    alias_start = manager_header.index('using GetTextureComponentType =')
    alias_end = manager_header.index(';', alias_start) + 1
    callbacks = 'struct ShaderManager { ' + manager_header[alias_start:alias_end] + ' };\n'
    for name in ('GraphicsEnvironment', 'ComputeEnvironment'):
        section = manager[manager.index('class ' + name):]
        callbacks += 'struct ' + name + ' : ComponentEnvironment { ShaderManager::GetTextureComponentType getTextureComponentType;\n'
        callbacks += function(section, '[[nodiscard]] bool IsTexturePixelFormatInteger(') + '\n'
        callbacks += function(section, '[[nodiscard]] Shader::TextureComponentType ReadTextureComponentType(') + '\n};\n'
    (tmp / 'callbacks.inc').write_text(callbacks)
    runtime = (GPU / 'interconnect/maxwell_3d/graphics_pipeline_state_accessor.cpp').read_text()
    file = (GPU / 'interconnect/common/file_pipeline_state_accessor.cpp').read_text()
    (tmp / 'accessors.inc').write_text(function(runtime, 'Shader::TextureComponentType RuntimeGraphicsPipelineStateAccessor::GetTextureComponentType(') + '\n' + function(file, 'Shader::TextureComponentType FilePipelineStateAccessor::GetTextureComponentType('))
    cache = (GPU / 'pipeline_cache_manager.cpp').read_text()
    assert 'static constexpr u32 Version{6}' in cache, 'numeric component serialization requires cache v6'
    start = textures.index('        auto &pool{texturePool.UpdateGet(ctx)};', textures.index('TextureView *Textures::GetTexture('))
    end = textures.index('        if (index >= textureHeaders.size())', start)
    cache_body = textures[start:end]
    if baseline == 'ignore-pool-generation':
        cache_body = cache_body.replace(' && cached.bindingGeneration == pool.bindingGeneration', '')
    (tmp / 'cache.inc').write_text(cache_body)
    (tmp / 'pool.inc').write_text(function(textures, 'void TexturePoolState::Flush(') + '\n' + function(textures, 'u64 Textures::GetBindingGeneration('))
    boost = Path(os.environ.get('BOOST_INCLUDE_DIR', Path('/usr/include')))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-Wno-unused-parameter', '-Wno-unknown-pragmas', '-Wno-dangling-reference', '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I' + str(tmp), '-I' + str(boost), '-I' + str(ROOT / 'app/libraries/vkhpp'), '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'), str(Path(__file__).with_name('tests.cpp')), '-o', str(tmp / 'tests')], check=True)
    subprocess.run([str(tmp / 'tests'), str(tmp / 'bundle.bin')], check=True)
