#!/usr/bin/env python3
"""Use exact cache methods and real VkRenderPass handles with validation enabled."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

root = Path(__file__).resolve().parents[2]
gpu = root / 'app/src/main/cpp/skyline/gpu'
with tempfile.TemporaryDirectory(prefix='strato-renderpass-vulkan-') as name:
    build = Path(name)
    header = (gpu / 'cache/renderpass_cache.h').read_text()
    (build / 'class.inc').write_text(function(header,'class RenderPassCache')+';')
    source = (gpu / 'cache/renderpass_cache.cpp').read_text()
    # Keep the entire production key implementation and GetRenderPass body.
    begin = source.index('    #define VEC_CPY')
    end = source.index('    vk::RenderPass RenderPassCache::GetRenderPass(')
    (build / 'production.inc').write_text(source[begin:end])
    (build / 'vulkan-production.inc').write_text(next(line for line in source.splitlines() if 'RenderPassCache::RenderPassCache(' in line)+'\n'+function(source,'vk::RenderPass RenderPassCache::GetRenderPass('))
    includes=[build,root/'app/libraries/vkhpp',Path(os.environ.get('VULKAN_HEADERS_INCLUDE_DIR',str(root/'app/libraries/vkhpp/Vulkan-Headers/include')))]
    includes+=list((root/'app/libraries/boost/libs').glob('*/include'))
    command=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS','-fsanitize=undefined','-fno-sanitize-recover=all',*[f'-I{p}' for p in includes],str(root/'tests/gpu_renderpass_cache/vulkan.cpp'),os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan'),'-o',str(build/'test')]
    subprocess.run(command+shlex.split(os.environ.get('CXXFLAGS','')),check=True)
    subprocess.run([str(build/'test')],check=True,timeout=60)
