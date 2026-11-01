#!/usr/bin/env python3
"""Production TrapManager/IntervalMap/NCE routing against real host mprotect.

Android ucontext records are synthetic on x86; no on-device ESR claim.
"""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
here=Path(__file__).resolve().parent
cpp=root/'app/src/main/cpp/skyline'
def read(path,baseline=None):
    return subprocess.check_output(['git','show',baseline+':app/src/main/cpp/skyline/'+path],cwd=root,text=True) if baseline else (cpp/path).read_text()
def call(source,signature,needle="TrapManager::TrapHandler("):
    start=source.index(needle,source.index(signature))
    end=source.index('(',start)+1;depth=1
    while depth:
        depth+=(source[end]=='(')-(source[end]==')');end+=1
    return source[start:end]
with tempfile.TemporaryDirectory(prefix='strato-readback-traps-') as td:
    td=Path(td)
    interval=read('common/interval_map.h').replace('#include "utils.h"','#include "host_types.h"').replace('#include "span.h"','')
    (td/'interval_map.h').write_text(interval)
    (td/'trap_manager.h').write_text(read('common/trap_manager.h'))
    source=read('common/trap_manager.cpp',os.environ.get('TRAP_BASELINE_REF'))
    (td/'trap.inc').write_text(source[source.index('namespace skyline {'):])
    source=read('nce.cpp',os.environ.get('NCE_BASELINE_REF'))
    (td/'routing.inc').write_text('bool RouteGuest(int signal,siginfo_t *info,ArmContext *ctx) {return '+call(source,'void NCE::SignalHandler(')+';}\n')
    source=read('gpu/buffer.cpp',os.environ.get('BUFFER_BASELINE_REF'))
    functions=[]
    for signature in ['void Buffer::SetupStagedTraps()', 'void Buffer::unlock()', 'void Buffer::WaitOnFence()', 'bool Buffer::PollFence()', 'void Buffer::Invalidate()', 'void Buffer::SynchronizeHost(', 'bool Buffer::SynchronizeGuest(', 'void Buffer::MarkGpuDirtyImplStaged()']:
        start=source.index(signature);end=source.index('{',start)+1;depth=1
        while depth:
            depth+=(source[end]=='{')-(source[end]=='}');end+=1
        functions.append(source[start:end])
    (td/'buffer.inc').write_text('namespace skyline::gpu {\n'+'\n'.join(functions)+'\n}')
    command=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-invalid-offsetof','-Wno-return-type','-O1','-g','-fsanitize='+os.environ.get('SANITIZERS','address,undefined'),'-fno-sanitize-recover=all','-I'+str(here),'-I'+str(td),'-I'+str(cpp),str(here/'tests.cpp'),'-o',str(td/'test')]
    if os.environ.get('VULKAN'):
        command += ['-DWITH_VULKAN','-I'+os.environ['VULKAN_HEADERS_INCLUDE_DIR'],os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan')]
    subprocess.run(command,check=True)
    subprocess.run([str(td/'test'),*os.sys.argv[1:]],check=True,timeout=60)
