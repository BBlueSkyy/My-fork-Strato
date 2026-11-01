#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile
here=Path(__file__).resolve().parent
root=here.parents[1];cpp=root/'app/src/main/cpp/skyline'
def read(path):
    ref=os.environ.get('BUFFER_BASELINE_REF')
    return subprocess.check_output(['git','show',ref+':app/src/main/cpp/skyline/'+path],cwd=root,text=True) if ref else (cpp/path).read_text()
def function(source,signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
with tempfile.TemporaryDirectory(prefix='strato-regional-buffer-') as directory:
    td=Path(directory)
    (td/'interval_map.h').write_text(read('common/interval_map.h').replace('#include "utils.h"','#include "host_types.h"').replace('#include "span.h"',''))
    (td/'trap_manager.h').write_text(read('common/trap_manager.h'))
    source=read('common/trap_manager.cpp');(td/'trap.inc').write_text(source[source.index('namespace skyline {'):])
    source=read('nce.cpp');start=source.index('TrapManager::TrapHandler(',source.index('void NCE::SignalHandler('));end=source.index('(',start)+1;depth=1
    while depth:depth+=(source[end]=='(')-(source[end]==')');end+=1
    (td/'routing.inc').write_text('bool RouteGuest(int signal,siginfo_t* info,ArmContext* ctx){return '+source[start:end]+';}')
    source=read('gpu/buffer.cpp');regional='void Buffer::RebuildStagedTraps()' in source
    signatures=['void Buffer::SetupStagedTraps()', 'void Buffer::WaitOnFence()', 'bool Buffer::PollFence()', 'void Buffer::Invalidate()', 'void Buffer::SynchronizeHost(', 'bool Buffer::SynchronizeGuest(', 'void Buffer::MarkGpuDirtyImplStaged(']
    signatures+=['void Buffer::RebuildStagedTraps()', 'void Buffer::PrepareStagedRange(', 'void Buffer::ProtectStagedRange(', 'void Buffer::MarkStagedCpuWrite(', 'void Buffer::RefreshStagedDirtyState()', 'bool Buffer::HasStagedCpuWrites()', 'void Buffer::SynchronizeHostRegion(', 'bool Buffer::SynchronizeGuestRegion(', 'void Buffer::SynchronizeHostForGpu()', 'void Buffer::MarkGpuDirtyImpl(', 'void Buffer::MarkGpuDirty(', 'bool Buffer::WriteImplStaged(', 'void Buffer::CopyFromImplStaged(', 'void Buffer::ReadImplStaged(', 'void Buffer::SynchronizeGuestImmediate('] if regional else []
    producers='void Buffer::AttachStagedProducerCycle(' in source
    if producers:
        signatures+=['void Buffer::AttachStagedProducerCycle(', 'Buffer::StagedReadbackDependency Buffer::GetStagedReadbackDependency(']
    if regional:
        (td/'coalesce.inc').write_text('namespace skyline::gpu {\n'+function(read('gpu/buffer_manager.cpp'),'BufferManager::LockedBuffer BufferManager::CoalesceBuffers(')+'\n}')
    (td/'buffer.inc').write_text('namespace skyline::gpu {\n'+'\n'.join(function(source,s) for s in signatures)+'\n}')
    command=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-return-type','-Wno-invalid-'+'offsetof','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DHAS_BUFFER_REGIONS='+str(int(regional)),'-DHAS_REGION_PRODUCERS='+str(int(producers)),'-I'+str(here),'-I'+str(td),'-I'+str(cpp),str(here/'tests.cpp'),'-o',str(td/'test')]
    if os.environ.get('VULKAN'):command+=['-DWITH_VULKAN','-I'+os.environ['VULKAN_HEADERS_INCLUDE_DIR'],os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan')]
    subprocess.run(command,check=True);subprocess.run([str(td/'test'),*os.sys.argv[1:]],check=True,timeout=60)
