#!/usr/bin/env python3
"""Validate the signal-safe register mailbox and enabled production scheduler hooks."""
from pathlib import Path
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix='strato-scheduler-diag-') as directory:
    binary = Path(directory) / 'mailbox'
    flags = ['-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-I' + str(ROOT / 'app/src/main/cpp/skyline')]
    sanitizer = os.environ.get('SANITIZE', 'undefined')
    if sanitizer:
        flags += ['-fsanitize=' + sanitizer, '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, str(HERE / 'diagnostic_mailbox.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
subprocess.run(['python3', str(HERE / 'run.py')], check=True,
               env={**os.environ, 'SCHEDULER_DIAGNOSTICS': '1'})
