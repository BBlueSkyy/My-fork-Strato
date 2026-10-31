#!/usr/bin/env python3
"""Compile real megabuffer decision/validation bodies with a CPU byte-copy allocator.
The allocator substitutes Vulkan allocation; policy and copied spans are production.
"""
from pathlib import Path
import subprocess,tempfile,os
R=Path(__file__).resolve().parents[2]; G=R/'app/src/main/cpp/skyline/gpu'
def function(s,signature):
 a=s.index(signature);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
s=(G/'buffer.cpp').read_text()
parts=[function(s,n)for n in ['void Buffer::ResetMegabufferState(', 'void Buffer::AdvanceSequence(', 'bool Buffer::ValidateMegaBufferViewImplDirect(', 'bool Buffer::ValidateMegaBufferViewImplStaged(', 'bool Buffer::ValidateMegaBufferView(', 'BufferBinding Buffer::TryMegaBufferView(']]
with tempfile.TemporaryDirectory(prefix='rejected-mega-')as td:
 d=Path(td);(d/'production.inc').write_text('namespace skyline::gpu {\n'+'\n'.join(parts)+'\n}')
 cmd=[os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize='+os.environ.get('SANITIZERS','undefined'),'-fno-sanitize-recover=all',f'-I{G}',f'-I{d}',str(Path(__file__).with_name('tests.cpp')),'-o',str(d/'test')]
 subprocess.run(cmd,check=True);subprocess.run([d/'test'],check=True)
