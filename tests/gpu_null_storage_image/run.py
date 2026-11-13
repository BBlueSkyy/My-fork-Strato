#!/usr/bin/env python3
"""Regression contract for null descriptors and shader-visible texture views."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
HERE = pathlib.Path(__file__).resolve().parent

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    sys.exit(1)

with tempfile.TemporaryDirectory(prefix='strato-null-storage-image-') as directory:
    binary = pathlib.Path(directory) / 'tests'
    sanitize = [] if os.environ.get('SANITIZE') == '0' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                    f'-I{GPU.parent}', str(HERE / 'tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

textures = (GPU / 'interconnect/common/textures.cpp').read_text()
get_texture = function(textures, 'TextureView *Textures::GetTexture(')
if get_texture.count('nullTextureView.get()') != 1 or 'CreateNullTexture(ctx)' not in get_texture:
    fail('GetTexture must centralize the shared null texture fallback')
reasons = re.findall(r'getNullTexture\(NullTextureReason::(\w+)\)', get_texture)
if sorted(reasons) != ['PoolIndexOutOfRange', 'Unmapped', 'Unmapped', 'UnsupportedFormat']:
    fail(f'GetTexture fallback reasons changed: {reasons}')

# Reserved TIC targets are null only when their guest GPU backing is absent.
# Test both compile-time classification and the actual sampled-image binding.
type_function = function(textures, 'Shader::TextureType Textures::GetTextureType(')
address_check = function(textures, 'static bool HasResidentTicAddress(')
for token in ('IsValidTicTextureType(rawTextureType)',
              'HasResidentTicAddress(ctx, textureHeader)',
              'ShouldNullInvalidTic(rawTextureType, resident)',
              'getNullTexture(NullTextureReason::Unmapped)'):
    if token not in get_texture:
        fail(f'invalid sampled TIC guard is missing {token}')
for token in ('ShouldNullInvalidTic(rawTextureType, resident)',
              'return Shader::TextureType::Color2D',
              'Invalid mapped TIC texture type'):
    if token not in type_function:
        fail(f'invalid shader TIC guard is missing {token}')
for token in ('GmmuAddressSpaceBits', 'LookupBlock(iova)', 'mapping.valid()'):
    if token not in address_check:
        fail(f'invalid TIC address checking is missing {token}')

null_texture = function(textures, 'static std::shared_ptr<TextureView> CreateNullTexture(')
if 'eStorage' in null_texture:
    fail('the shared sampled null texture must not become writable storage')

pipeline = (GPU / 'interconnect/common/pipeline.inc').read_text()
sampled_binding = function(pipeline, 'GetTextureBinding(InterconnectContext &ctx')
storage_binding = function(pipeline, 'GetImageBinding(InterconnectContext &ctx')
for token in ('IsNullBindlessHandle(handle.raw)', 'GetNullSampler(ctx)', 'supportsNullDescriptor', 'VK_NULL_HANDLE'):
    if token not in sampled_binding:
        fail(f'sampled null descriptor path is missing {token}')
if 'ResolveStorageImageNullAction' not in storage_binding:
    fail('storage null descriptors must use the explicit null policy')

samplers = (GPU / 'interconnect/common/samplers.cpp').read_text()
null_sampler = function(samplers, 'vk::raii::Sampler *Samplers::GetNullSampler(')
if 'vk::Filter::eLinear' not in null_sampler or 'std::make_unique<vk::raii::Sampler>' not in null_sampler:
    fail('null bindless sampler must remain a real Vulkan sampler')

executor = (GPU / 'interconnect/command_executor.cpp').read_text()
create_rp = function(executor, 'bool CommandExecutor::CreateRenderPassWithSubpass(')
if '!view || view->texture->ValidateRenderPassUsage' not in create_rp:
    fail('render-pass validation must accept null sampled descriptors')
if not re.search(r'for \(auto view : sampledImages\)\s*if \(view\)\s*view->texture->UpdateRenderPassUsage', create_rp):
    fail('render-pass usage tracking must skip null sampled descriptors')

for token in (
    'case TextureImageControl::TextureType::e2DNoMipmap: {',
    'shaderType == Shader::TextureType::ColorArray2D',
    'guest.viewType = arrayView ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D',
    'guest.layerCount = 1',
):
    if token not in textures:
        fail(f'e2DNoMipmap array-view support is missing {token}')

print('null descriptor / texture view regression contract: ok')
