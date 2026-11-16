#!/usr/bin/env python3
"""Compile the standalone production diagnostics (no Android/Vulkan facade)."""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='strato-staging-stats-') as path:
    binary = pathlib.Path(path) / 'stats'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g', '-pthread',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I' + str(ROOT / 'app/src/main/cpp/skyline'),
                    str(ROOT / 'tests/gpu_staging_retirement/stats_tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
