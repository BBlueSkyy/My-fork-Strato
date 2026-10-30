#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    binary = str(Path(tmp) / 'readback_diagnostics')
    subprocess.run(['c++', '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root / 'app/src/main/cpp/skyline/gpu'),
                    str(Path(__file__).with_name('tests.cpp')), '-o', binary], check=True)
    subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
print('PASS: classification, concurrent accumulation, actual-use separation and 5s reporting gate')
