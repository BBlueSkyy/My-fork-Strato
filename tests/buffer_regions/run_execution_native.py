#!/usr/bin/env python3
"""Actual FenceCycle and producer HOST_READ barrier on a native Vulkan queue."""
import os,subprocess,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
root=here.parents[1];cpp=root/'app/src/main/cpp/skyline'
with tempfile.TemporaryDirectory(prefix='strato-execution-native-') as directory:
    td=Path(directory);(td/'common').mkdir()
    stubs=root/'tests/gpu_submission/stubs'
    for path in ('common.h','common/spin_lock.h'):(td/path).write_text((stubs/path).read_text())
    (td/'common/atomic_forward_list.h').write_text((cpp/'common/atomic_forward_list.h').read_text())
    source=(cpp/'gpu/interconnect/command_executor.cpp').read_text();start=source.index('    static void RecordStagedReadbackBarrier(');end=source.index('{',start)+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    (td/'host_barrier.inc').write_text(source[start:end])
    cmd=[os.environ.get('CXX','c++'),'-std=c++20','-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS','-DVULKAN_HPP_NO_SPACESHIP_OPERATOR','-pthread','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(td),'-I'+os.environ['VULKAN_HPP_INCLUDE_DIR'],'-I'+os.environ['VULKAN_HEADERS_INCLUDE_DIR'],'-I'+str(cpp),str(here/'execution_native.cpp'),os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan'),'-ldl','-o',str(td/'test')]
    subprocess.run(cmd,check=True);subprocess.run([str(td/'test')],check=True,timeout=45)
