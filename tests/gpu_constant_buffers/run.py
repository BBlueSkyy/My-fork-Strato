#!/usr/bin/env python3
"""Compile production selector/cache/Load/Bind/Read bodies with real GMMU mappings."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'

def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='strato-constant-buffers-') as directory:
    tmp = pathlib.Path(directory)
    # Reuse the common, dependency-only facade from the GMMU tests. All mapping
    # algorithms below come from the real production address-space implementation.
    preamble = (ROOT / 'tests/gmmu/tests.cpp').read_text().split('using namespace skyline;')[0]
    preamble = preamble.replace('bool valid() const', 'template<class U> span<U> cast() { return {reinterpret_cast<U *>(this->data()), this->size_bytes()/sizeof(U)}; }\n    bool valid() const')
    (tmp / 'preamble.inc').write_text(preamble)
    for name in ('address_space.h', 'address_space.inc', 'segment_table.h'):
        source = (CPP / 'common' / name).read_text()
        (tmp / name).write_text('\n'.join(line for line in source.splitlines() if not line.startswith('#include')))
    dirty = (CPP / 'common/dirty_tracking.h').read_text()
    (tmp / 'dirty.inc').write_text('\n'.join(extract(dirty, signature) + ';' for signature in (
        'template<typename T>\n    class BoundSubresource', 'class ManualDirty {',
        'class RefreshableManualDirty', 'class CachedManualDirty',
        'template<typename T> requires (std::is_base_of_v<ManualDirty, T>)')))
    header = (CPP / 'gpu/interconnect/common/common.h').read_text()
    source = (CPP / 'gpu/interconnect/common/common.cpp').read_text()
    (tmp / 'cache.inc').write_text(extract(header, 'class CachedMappedBufferView') + ';\n' +
        '\n'.join(extract(source, sig) for sig in ('void CachedMappedBufferView::Update(', 'void CachedMappedBufferView::PurgeCaches(')))
    # The Read declaration and full implementation are also production code.
    (tmp / 'read.inc').write_text(extract(header, 'struct ConstantBuffer {') + ';\n' + extract(source, 'void ConstantBuffer::Read('))
    (tmp / 'engine_types.inc').write_text(extract((CPP / 'soc/gm20b/engines/engine.h').read_text(), 'struct Address {') + ';\n' + extract((CPP / 'soc/gm20b/engines/maxwell/types.h').read_text(), 'struct ConstantBufferSelector {') + ';')
    cb_header = (CPP / 'gpu/interconnect/maxwell_3d/constant_buffers.h').read_text()
    cb_source = (CPP / 'gpu/interconnect/maxwell_3d/constant_buffers.cpp').read_text()
    baseline = os.environ.get('CONSTANT_BUFFER_BASELINE_REF')
    if baseline:
        for name in ('constant_buffers.h', 'constant_buffers.cpp'):
            value = subprocess.check_output(['git', 'show', baseline + ':app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/' + name], cwd=ROOT, text=True)
            if name.endswith('.h'): cb_header = value
            else: cb_source = value
    (tmp / 'selector.inc').write_text('\n'.join(line for line in cb_header.splitlines() if not line.startswith(('#include', '#pragma'))) + '\n' + '\n'.join(line for line in cb_source.splitlines() if not line.startswith('#include')))
    binary = tmp / 'tests'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-Wno-unused-parameter', '-Wno-unused-variable', '-D_GLIBCXX_ASSERTIONS', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-pthread', '-I' + str(tmp), '-I' + os.environ.get('BOOST_INCLUDE_DIR', '/usr/include'),
        str(ROOT / 'tests/gpu_constant_buffers/tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *os.sys.argv[1:]], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
