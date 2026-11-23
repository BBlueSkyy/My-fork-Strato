#!/usr/bin/env python3
"""Compile the production UsageTracker with ASan/UBSan, without Android headers."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='strato-gpu-usage-') as directory:
    directory = pathlib.Path(directory)
    # The legacy tracker includes IntervalList -> Android span utilities. Keep
    # the exact interval implementation for the baseline negative control.
    common = directory / 'common'
    common.mkdir()
    source = (ROOT / 'app/src/main/cpp/skyline/common/interval_list.h').read_text()
    (common / 'interval_list.h').write_text(
        '#pragma once\n#include <algorithm>\n#include <vector>\n#include <span>\n'
        '#include <cstdint>\n#include <utility>\n'
        'namespace skyline { using u8 = uint8_t; template<class T> using span = std::span<T>; }\n'
        + source[source.index('namespace skyline'):])
    executable = directory / 'tests'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(directory),
                    '-I' + str(ROOT / 'app/src/main/cpp/skyline'),
                    str(ROOT / 'tests/gpu_usage/tests.cpp'), '-o', str(executable)], check=True)
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run([str(executable)], check=True, env=environment)
