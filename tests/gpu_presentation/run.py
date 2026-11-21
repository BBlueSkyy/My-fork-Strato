#!/usr/bin/env python3
"""Run complete production swapchain retirement/update methods with host facades."""
import os
import sys
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
path = 'app/src/main/cpp/skyline/gpu/presentation_engine.cpp'
ref = os.environ.get('PRESENTATION_BASELINE_REF')
source = subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT, text=True) if ref else (ROOT / path).read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='strato-presentation-') as directory:
    directory = Path(directory)
    methods = ['void PresentationEngine::UpdateSurface(', 'void PresentationEngine::UpdateSwapchain(']
    if 'void PresentationEngine::DestroySwapchain(' in source:
        methods.insert(0, 'void PresentationEngine::DestroySwapchain(')
    (directory / 'production.inc').write_text('\n'.join(function(signature) for signature in methods))
    command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-I' + str(directory),
               '-I' + str(ROOT / 'app/libraries/vkhpp'),
               '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
               str(HERE / 'tests.cpp'), '-o', str(directory / 'tests')]
    sanitizer = os.environ.get('SANITIZE', 'undefined')
    if sanitizer:
        command += ['-fsanitize=' + sanitizer, '-fno-sanitize-recover=all']
    subprocess.run(command, check=True)
    for _ in range(int(os.environ.get('STRESS_RUNS', '1'))):
        subprocess.run([str(directory / 'tests')], check=True, timeout=15)

# Keep the shared presentation queue's object lifetime under the same CI gate.
subprocess.run([sys.executable, str(HERE / 'run_queue.py')], check=True)
