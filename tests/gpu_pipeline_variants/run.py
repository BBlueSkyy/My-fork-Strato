#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'app/src/main/cpp/skyline/gpu/interconnect'
def function(text,mark):
 i=text.index(mark);j=text.index('{',i)+1;depth=1
 while depth:depth+=(text[j]=='{')-(text[j]=='}');j+=1
 return text[i:j]
g=(BASE/'maxwell_3d/pipeline_manager.cpp').read_text()
c=(BASE/'kepler_compute/pipeline_manager.h').read_text()
old=os.environ.get('PIPELINE_VARIANTS_BASELINE_REF')
if old:
 g=subprocess.check_output(['git','show',old+':app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp'],cwd=ROOT,text=True)
 c=subprocess.check_output(['git','show',old+':app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.h'],cwd=ROOT,text=True)
 g=g.replace('Pipeline *Pipeline::LookupNext(const PackedPipelineState &packedState)', 'Pipeline *Pipeline::LookupNext(const PackedPipelineState &packedState, InterconnectContext &ctx, Textures &textures, ConstantBufferSet &constantBuffers)')
with tempfile.TemporaryDirectory(prefix='strato-variants-') as d:
 d=Path(d)
 (d/'graphics.inc').write_text(function(g,'Pipeline *PipelineManager::FindOrCreate(')+'\n'+function(g,'Pipeline *Pipeline::LookupNext('))
 (d/'compute.inc').write_text(function(c,'Pipeline *FindOrCreate(').replace('Pipeline *FindOrCreate(','Pipeline *PipelineManager::FindOrCreate(',1))
 subprocess.run(['c++',*(['-DOLD_VARIANT_CACHE'] if old else []),'-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(d),str(Path(__file__).with_name('tests.cpp')),'-o',str(d/'tests')],check=True)
 subprocess.run([str(d/'tests')],check=True)
