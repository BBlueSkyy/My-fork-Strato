#!/usr/bin/env python3
"""Exercise production GMMU translation bodies with resident/sparse block tables."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def function(source, signature, defaults=False):
    start = source.index(signature)
    brace = source.index(') {', start) + 2 if defaults else source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

header = (root/'app/src/main/cpp/skyline/common/address_space.h').read_text()
source = (root/'app/src/main/cpp/skyline/common/address_space.inc').read_text()
with tempfile.TemporaryDirectory(prefix='strato-region-residency-') as directory:
    directory = Path(directory)
    (directory/'translation.inc').write_text(function(header, 'std::pair<span<u8>, size_t> LookupBlockLocked(', True) + '\n' +
        function(header, 'TranslatedAddressRange TranslateRange(', True))
    (directory/'slow.inc').write_text(function(source, 'MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl('))
    exe = directory/'tests'
    subprocess.run([os.environ.get('CXX','c++'), '-std=c++20','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(directory),
        str(Path(__file__).with_name('tests.cpp')),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
