#!/usr/bin/env python3
"""Compile the production scheduler methods and adaptive condition variable on Linux.
Only guest execution, signals/timers and the AArch64 spin instruction are facades.
SCHEDULER_BASELINE_REF=origin/master reproduces the pre-fix failures.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASE = 'app/src/main/cpp/skyline/'
def source(path):
    ref = os.environ.get('SCHEDULER_BASELINE_REF')
    return subprocess.check_output(['git', 'show', ref + ':' + BASE + path], cwd=ROOT, text=True) if ref else (ROOT / BASE / path).read_text()
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
with tempfile.TemporaryDirectory(prefix='strato-scheduler-') as tmp:
    tmp = Path(tmp)
    header = source('kernel/scheduler.h')
    header = '\n'.join(line for line in header.splitlines() if not line.startswith('#include'))
    # Inspect queues in tests without adding any test API to production.
    header = header.replace('private:', 'public:')
    (tmp / 'scheduler.inc').write_text(header)
    cpp = source('kernel/scheduler.cpp')
    start = cpp.index('    Scheduler::CoreContext &Scheduler::GetOptimalCoreForThread')
    (tmp / 'methods.inc').write_text('namespace skyline::kernel {\n' + cpp[start:])
    spin = source('common/spin_lock.h')
    (tmp / 'condition.inc').write_text(function(spin, 'class AdaptiveSingleWaiterConditionVariable') + ';')
    svc = source('kernel/svc.cpp')
    (tmp / 'svc.inc').write_text('namespace skyline::kernel::svc {\n' + function(svc, 'void SleepThread(') + '\n}')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-O1', '-g']
    sanitizer = os.environ.get('SANITIZE', 'undefined')
    if sanitizer:
        flags += ['-fsanitize=' + sanitizer, '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(tmp), str(HERE / 'tests.cpp'), str(HERE / 'mutex_checks.cpp'),
                    '-Wl,--wrap=pthread_mutex_lock,--wrap=pthread_mutex_trylock,--wrap=pthread_mutex_unlock', '-o', str(tmp / 'tests')], check=True)
    runs = int(os.environ.get('STRESS_RUNS', '1'))
    for _ in range(runs):
        result = subprocess.run([str(tmp / 'tests'), *os.sys.argv[1:]],
                                text=True, stdout=subprocess.PIPE if runs > 1 else None,
                                stderr=subprocess.STDOUT, timeout=45)
        if result.returncode:
            if result.stdout:
                print(result.stdout, end='')
            result.check_returncode()
    if runs > 1:
        print(f'PASS {runs} complete scheduler suite repetitions')
