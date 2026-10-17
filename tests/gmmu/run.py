#!/usr/bin/env python3
"""Compile production translation/map/table bodies; replace Android includes only."""
import os, pathlib, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='strato-gmmu-') as tmp:
    tmp = pathlib.Path(tmp)
    for name in ('address_space.h', 'address_space.inc', 'segment_table.h'):
        path = 'app/src/main/cpp/skyline/common/' + name
        baseline = os.environ.get('GMMU_BASELINE_REF')
        source = subprocess.check_output(['git','show',baseline+':'+path],cwd=ROOT,text=True) if baseline else (ROOT/path).read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        (tmp / name).write_text(source)
    def extract(source, signature):
        start = source.index(signature)
        end = source.index('{', start) + 1
        depth = 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]
    cache_header = (ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/common.h').read_text()
    cache_source = (ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/common.cpp').read_text()
    (tmp/'buffer_class.inc').write_text(extract(cache_header,'class CachedMappedBufferView')+';')
    (tmp/'buffer_update.inc').write_text('\n'.join(extract(cache_source,signature) for signature in
        ('void CachedMappedBufferView::Update(', 'void CachedMappedBufferView::PurgeCaches(')))
    compiler = os.environ.get('CXX', 'c++')
    binary = tmp / 'tests'
    subprocess.run([compiler, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-reorder', '-Wno-self-assign', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-pthread', '-I'+str(tmp),
        '-I'+os.environ.get('BOOST_INCLUDE_DIR','/usr/include'),
        str(ROOT/'tests/gmmu/tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *os.sys.argv[1:]], check=True,
        env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
