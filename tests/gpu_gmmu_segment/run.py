#!/usr/bin/env python3
"""Compare the real two-level table against independent mapped-page bytes."""
import os
from pathlib import Path
import subprocess
import tempfile
from gmmu_segment_fixture import write_fixture, ROOT
with tempfile.TemporaryDirectory(prefix='strato-gmmu-segment-') as temp:
    temp=Path(temp)
    write_fixture(temp,ref=os.environ.get('GMMU_SEGMENT_REF'))
    exe=temp/'tests'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'app/src/main/cpp/skyline'),'-I'+str(temp),str(Path(__file__).with_name('tests.cpp')),'-o',str(exe)],check=True)
    env=dict(os.environ,ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS','')+':detect_leaks=0')
    subprocess.run([str(exe)],check=True,env=env,timeout=30)
