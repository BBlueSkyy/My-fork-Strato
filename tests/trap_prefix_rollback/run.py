#!/usr/bin/env python3
"""Exercise real trap/mprotect rollback without Android or GPU transport."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = ROOT / 'app/src/main/cpp/skyline/common'
BASELINE = os.environ.get('TRAP_BASELINE_REF')


def without_includes(path):
    source = subprocess.check_output(['git', 'show', BASELINE + ':' + str(path.relative_to(ROOT))],
        cwd=ROOT, text=True) if BASELINE else path.read_text()
    return '\n'.join(line for line in source.splitlines()
                     if not line.startswith('#include') and not line.startswith('#pragma once'))


with tempfile.TemporaryDirectory(prefix='strato-trap-rollback-') as temporary:
    temporary = pathlib.Path(temporary)
    (temporary / 'interval.inc').write_text(without_includes(COMMON / 'interval_map.h'))
    (temporary / 'trap.inc').write_text('\n'.join(without_includes(COMMON / name)
        for name in ('trap_manager.h', 'trap_manager.cpp')))
    binary = temporary / 'tests'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-invalid-offsetof', '-pthread', '-I' + str(temporary), str(pathlib.Path(__file__).with_name('tests.cpp')),
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
