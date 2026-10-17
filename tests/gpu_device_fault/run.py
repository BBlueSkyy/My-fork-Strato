#!/usr/bin/env python3
"""Build bounded, one-shot post-loss diagnostics tests with Vulkan headers, no GPU required."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADERS = Path(os.environ.get('VULKAN_HEADERS', ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))
with tempfile.TemporaryDirectory(prefix='strato-device-fault-') as directory:
    binary = Path(directory) / 'tests'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I' + str(HEADERS), '-I' + str(ROOT / 'app/src/main/cpp/skyline'),
                    str(Path(__file__).with_name('tests.cpp')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
print('PASS post-loss fault capture: feature gate, once-only concurrency, failure, bounded incomplete and empty reports')
