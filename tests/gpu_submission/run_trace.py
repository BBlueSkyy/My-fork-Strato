#!/usr/bin/env python3
"""Production FenceCycle with controlled host waits or native Vulkan validation."""
import os
from pathlib import Path
import subprocess
import tempfile
here=Path(__file__).resolve().parent
cpp=here.parents[1]/'app/src/main/cpp/skyline'
with tempfile.TemporaryDirectory(prefix='strato-fence-trace-') as td:
    td=Path(td)
    (td/'common').mkdir()
    (td/'common/atomic_forward_list.h').write_text((cpp/'common/atomic_forward_list.h').read_text())
    binary=str(td/'trace')
    includes=[]
    native=bool(os.environ.get('VULKAN'))
    if native:
        includes=['-I'+os.environ['VULKAN_HPP_INCLUDE_DIR'],'-I'+os.environ['VULKAN_HEADERS_INCLUDE_DIR']]
    source=here/('fence_trace_native.cpp' if native else 'fence_trace.cpp')
    command=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror',
             '-DSTRATO_BUFFER_READBACK_PROBE=1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wl,--wrap=clock_gettime',
             *includes,'-I'+str(td),'-I'+str(here/'stubs'),'-I'+str(cpp),str(source),'-o',binary]
    if native:
        command += [os.environ.get('VULKAN_LOADER_LIBRARY','-lvulkan'),'-ldl']
    subprocess.run(command,check=True)
    subprocess.run([binary],check=True,timeout=30)
