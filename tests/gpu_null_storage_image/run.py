#!/usr/bin/env python3
"""Check the null storage image policy: production header on the host plus source-level contract guards."""
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


# 1. Production header, compiled and executed
with tempfile.TemporaryDirectory(prefix='strato-null-storage-image-') as directory:
    binary = pathlib.Path(directory) / 'tests'
    sanitize = [] if os.environ.get('SANITIZE') == '0' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                    f'-I{GPU.parent}', str(HERE / 'tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# 2. Every path that hands out the shared null texture must say why, or consumers cannot tell stale
#    TICs from surfaces the guest expects to exist.
textures = (GPU / 'interconnect/common/textures.cpp').read_text()
get_texture = function(textures, 'TextureView *Textures::GetTexture(')
if get_texture.count('nullTextureView.get()') != 1 or 'CreateNullTexture(ctx)' not in get_texture:
    fail('GetTexture must hand out the null texture from exactly one place (the getNullTexture lambda)')
reasons = re.findall(r'getNullTexture\(NullTextureReason::(\w+)\)', get_texture)
if sorted(reasons) != ['PoolIndexOutOfRange', 'Unmapped', 'UnsupportedFormat']:
    fail(f'GetTexture fallbacks must each report their reason, found {reasons}')

# 3. The shared null texture is a sampled-only stand-in: it must never be widened into a storage image
#    (a writable shared 1x1 image leaks stores between bindings), storage goes through the policy instead.
null_texture = function(textures, 'static std::shared_ptr<TextureView> CreateNullTexture(')
if 'eStorage' in null_texture:
    fail('CreateNullTexture must not gain storage usage, use ResolveStorageImageNullAction / null descriptors')

pipeline = (GPU / 'interconnect/common/pipeline.inc').read_text()
image_binding = function(pipeline, 'GetImageBinding(InterconnectContext &ctx')
if 'ResolveStorageImageNullAction' not in image_binding:
    fail('GetImageBinding must consult ResolveStorageImageNullAction')
if 'AttachTexture' in image_binding[:image_binding.index('ctx.executor.AttachTexture(texture);')]:
    fail('the null texture must not be attached before the policy decision')

# 4. Maxwell's 0xFFFFFFFF bindless sentinel must become a complete sampled null
#    resource: valid sampler + null image view. A null sampler is not a Vulkan null descriptor.
sampled_binding = function(pipeline, 'GetTextureBinding(InterconnectContext &ctx')
required_sampled = ('IsNullBindlessHandle(handle.raw)', 'GetNullSampler(ctx)',
                    'supportsNullDescriptor', 'VK_NULL_HANDLE')
for token in required_sampled:
    if token not in sampled_binding:
        fail(f'sampled null bindless path is missing {token}')
if sampled_binding.index('GetNullSampler(ctx)') > sampled_binding.index('GetSampler(ctx, handle.samplerIndex'):
    fail('0xFFFFFFFF must select the valid null sampler before normal TSC lookup')

samplers = (GPU / 'interconnect/common/samplers.cpp').read_text()
null_sampler = function(samplers, 'vk::raii::Sampler *Samplers::GetNullSampler(')
if 'vk::Filter::eLinear' not in null_sampler or 'std::make_unique<vk::raii::Sampler>' not in null_sampler:
    fail('null bindless sampler must be a real deterministic Vulkan sampler')

if 'index == NullBindlessTextureIndex' not in textures:
    fail('shader metadata lookup must recognize the 0xFFFFF TIC half without warning')

print('null storage/sampled image source contract: ok')
