#!/usr/bin/env python3
"""Compile production translation/map/table bodies; replace Android includes only."""
import os, pathlib, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='strato-gmmu-') as tmp:
    tmp = pathlib.Path(tmp)
    for name in ('address_space.h', 'address_space.inc', 'segment_table.h'):
        path = 'app/src/main/cpp/skyline/common/' + name
        baseline = os.environ.get('GMMU_TABLE_BASELINE_REF') if name == 'segment_table.h' else None
        source = subprocess.check_output(['git','show',baseline+':'+path],cwd=ROOT,text=True) if baseline else (ROOT/path).read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        if name == 'segment_table.h':
            source=source.replace('void Set(size_t start, size_t end, SegmentType segment) {',
                'void Set(size_t start, size_t end, SegmentType segment) { segmentWriteBytes += end - start;')
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
    texture_source=(ROOT/'app/src/main/cpp/skyline/gpu/texture/texture.cpp').read_text()
    texture_baseline=os.environ.get('GMMU_TEXTURE_BASELINE_REF')
    if texture_baseline:
        texture_source=subprocess.check_output(['git','show',texture_baseline+':app/src/main/cpp/skyline/gpu/texture/texture.cpp'],cwd=ROOT,text=True)
    guest_header=(ROOT/'app/src/main/cpp/skyline/gpu/texture/texture.h').read_text()
    (tmp/'guest_set.inc').write_text(extract(guest_header,'template<typename MemoryManager>'))
    (tmp/'guest_valid.inc').write_text(extract(texture_source,'bool GuestTexture::MappingsValid('))
    (tmp/'mark_dirty.inc').write_text(extract(texture_source,'void Texture::MarkGpuDirty('))
    (tmp/'buffer_update.inc').write_text('\n'.join(extract(cache_source,signature) for signature in
        ('void CachedMappedBufferView::Update(', 'void CachedMappedBufferView::PurgeCaches(')))
    executor=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp').read_text()
    signature='void CommandExecutor::NotifyMemoryMappingChange('
    (tmp/'mapping_notify.inc').write_text(extract(executor,signature) if signature in executor else '')
    shader_header=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/shader_cache.h').read_text()
    shader_source=(ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/shader_cache.cpp').read_text()
    shader_class=extract(shader_header,'class ShaderCache').replace('private:', 'public:')
    shader_class=shader_class[:-1]+'void SelectMirrorForTest(InterconnectContext&, span<u8>);\n};'
    (tmp/'shader_class.inc').write_text(shader_class)
    selection=extract(shader_source,'if (!mirrorBlock.valid()')
    (tmp/'shader_select.inc').write_text('void ShaderCache::SelectMirrorForTest(InterconnectContext &ctx, span<u8> blockMapping) {'+selection+'}')
    compiler = os.environ.get('CXX', 'c++')
    binary = tmp / 'tests'
    subprocess.run([compiler, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-D_GLIBCXX_ASSERTIONS', '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-reorder', '-Wno-self-assign', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-pthread', '-I'+str(tmp),
        '-I'+os.environ.get('BOOST_INCLUDE_DIR','/usr/include'),
        str(ROOT/'tests/gmmu/tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *os.sys.argv[1:]], check=True,
        env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
