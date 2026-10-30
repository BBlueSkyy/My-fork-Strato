#!/usr/bin/env python3
"""ASan/UBSan regression checks using exact production translation/query bodies."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SKYLINE = ROOT / 'app/src/main/cpp/skyline'


def body(source, signature):
    start = source.index('{', source.index(signature))
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix='strato-sparse-') as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'translate.inc').write_text(body((SKYLINE / 'common/address_space.inc').read_text(), '::TranslateRangeImpl('))
    header = (SKYLINE / 'common/address_space.h').read_text()
    has_query = 'bool IsFullySparseRange(' in header
    (tmp / 'query.inc').write_text(body(header, 'bool IsFullySparseRange(') if has_query else '{ return false; }')
    (tmp / 'fastpath.inc').write_text(body(header, 'TranslatedAddressRange TranslateRange('))
    (tmp / 'lookup.inc').write_text(body(header, 'std::pair<span<u8>, size_t> LookupBlockLocked('))
    (tmp / 'clip.inc').write_text(body(header, 'void ClipPrecedingSegmentExtent('))
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie']
    exe = tmp / 'mapping-tests'
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(tmp),
                    str(ROOT / 'tests/gpu_sparse/mapping.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    assert has_query, 'whole-range sparse classification is missing'
    exe = tmp / 'size-tests'
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(SKYLINE),
                    str(ROOT / 'tests/gpu_sparse/sizing.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
