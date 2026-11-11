#!/usr/bin/env python3
"""Compile the complete production megabuffer implementation and real FenceCycle.
Only Vulkan fence operations and GPU memory allocation are host facades.
Use --baseline REF to run the same regression against an earlier revision.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--baseline')
parser.add_argument('--benchmark', action='store_true')
parser.add_argument('--repeat', type=int, default=1)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='strato-megabuffer-gc-') as path:
    build = pathlib.Path(path)
    (build / 'common').mkdir()
    for name in ('megabuffer.h', 'megabuffer.cpp', 'fence_cycle.h'):
        relative = 'app/src/main/cpp/skyline/gpu/' + name
        source = subprocess.check_output(['git', 'show', args.baseline + ':' + relative], cwd=ROOT, text=True) if args.baseline else (ROOT / relative).read_text()
        if name == 'megabuffer.h':
            implemented = 'CollectIdleChunk' in source
            source = source.replace('private:', 'public:')  # Test-only visibility, unchanged bodies.
        (build / name).write_text(source)
    # Use the production lock-free dependency list (not the submission test facade).
    shutil.copy(ROOT / 'app/src/main/cpp/skyline/common/atomic_forward_list.h', build / 'common')
    for name in ('memory_manager.h', 'gpu.h'):
        (build / name).write_text('#pragma once\n#include "support.h"\n#include "fence_cycle.h"\n')
    command = [os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-I' + str(build), '-I' + str(HERE), '-I' + str(ROOT / 'tests/gpu_submission/stubs'),
               str(build / 'megabuffer.cpp'), str(HERE / 'tests.cpp'), '-o', str(build / 'tests')]
    if implemented:
        command += ['-DGC_IMPLEMENTED']
    if args.benchmark:
        command += ['-O3', '-DNDEBUG']
    else:
        command += ['-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZE', 'address,undefined'), '-fno-sanitize-recover=all']
    subprocess.run(command, check=True)
    for _ in range(args.repeat):
        subprocess.run([str(build / 'tests'), *(['--benchmark'] if args.benchmark else [])], check=True, timeout=60,
                       env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
