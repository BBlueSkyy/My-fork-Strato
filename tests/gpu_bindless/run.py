#!/usr/bin/env python3
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/pipeline.inc').read_text()
def extract(start, text=None):
    global source
    original=source
    if text is not None: source=text
    i=source.index(start); j=source.index('{',i)+1; depth=1
    while depth:
        depth+=(source[j]=='{')-(source[j]=='}'); j+=1
    result=source[i:j]
    source=original
    return result
with tempfile.TemporaryDirectory(prefix='strato-bindless-') as d:
    d=Path(d)
    shader=Path(os.environ.get('SHADER_COMPILER_ROOT',ROOT/'app/libraries/shader-compiler'))
    if not (shader/'shader_info.h').exists(): shader=ROOT.parent/'shader-compiler'
    info=(shader/'shader_info.h').read_text()
    helpers='namespace Shader {\n'+extract('constexpr u32 ShiftTextureHandle',info)+'\ntemplate <typename Descriptor>\n'+extract('constexpr u32 EvaluateTextureHandle',info)+'\n}\n'
    (d/'production.inc').write_text(helpers+extract('union BindlessHandle')+';\n'+extract('static vk::DescriptorType ResolveDescriptorType')+'\n'+extract('static bool HasSamplerlessDescriptors')+'\n'+extract('static BindlessHandle ReadBindlessHandle')+'\n'+extract('static std::pair<vk::DescriptorImageInfo, TextureView *> GetTextureBinding')+'\n'+extract('static bool TextureComponentsMatch'))
    graphics=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
    compute=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.cpp').read_text()
    draw=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    start=draw.index('         const auto samplerGeneration{',draw.index('void Maxwell3D::PrepareDraw('))
    end=draw.index('         activeState.Update(',start)
    (d/'generation.inc').write_text(draw[start:end])
    (d/'layout.inc').write_text(extract('auto pushBindings{',graphics)+';')
    (d/'writes.inc').write_text(extract('auto writeImageDescs{',graphics)+';')
    (d/'compute_layout.inc').write_text(extract('auto pushBindings{',compute)+';')
    (d/'compute_writes.inc').write_text(extract('auto writeImageDescs{',compute)+';')
    subprocess.run(['c++','-std=c++20','-DVULKAN_HPP_NO_CONSTRUCTORS','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(d),'-I'+os.environ.get('VULKAN_HPP_INCLUDE',str(ROOT/'app/libraries/vkhpp')),'-I'+os.environ.get('VULKAN_HEADERS_INCLUDE',str(ROOT/'app/libraries/vkhpp/Vulkan-Headers/include')),str(Path(__file__).with_name('tests.cpp')),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
