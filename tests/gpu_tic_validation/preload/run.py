#!/usr/bin/env python3
"""Execute the production preload constructor with deferred-resource facades."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
source = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
start = source.index('PipelineManager::PipelineManager(GPU &gpu, JvmManager &jvm)')
cursor = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[cursor] == '{') - (source[cursor] == '}')
    cursor += 1
with tempfile.TemporaryDirectory(prefix='strato-preload-validation-') as directory:
    build = pathlib.Path(directory)
    (build / 'constructor.inc').write_text(source[start:cursor])
    binary = build / 'preload'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + str(build),
                    str(HERE / 'tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
