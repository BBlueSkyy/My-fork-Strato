#!/usr/bin/env python3
"""Run production cached GPU-buffer translation with host transport facades."""
import os, pathlib, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
with tempfile.TemporaryDirectory(prefix='strato-gpu-buffers-') as directory:
    tmp = pathlib.Path(directory)
    header = (ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/common.h').read_text()
    source = (ROOT/'app/src/main/cpp/skyline/gpu/interconnect/common/common.cpp').read_text()
    (tmp/'cache.inc').write_text(extract(header, 'class CachedMappedBufferView')+';\n'+ '\n'.join(extract(source,sig) for sig in ('void CachedMappedBufferView::Update(', 'void CachedMappedBufferView::PurgeCaches(')))
    buffer = (ROOT/'app/src/main/cpp/skyline/gpu/buffer.cpp').read_text()
    manager = (ROOT/'app/src/main/cpp/skyline/gpu/buffer_manager.cpp').read_text()
    buffer_header = (ROOT/'app/src/main/cpp/skyline/gpu/buffer.h').read_text()
    (tmp/'delegates.inc').write_text(extract(buffer_header,'class BufferDelegate {')+';\n'+next(line.strip() for line in buffer.splitlines() if 'BufferDelegate::BufferDelegate(' in line)+'\n'+
        '\n'.join(extract(buffer,sig) for sig in ('Buffer *BufferDelegate::GetBuffer(',
        'void BufferDelegate::Link(', 'vk::DeviceSize BufferDelegate::GetOffset(')))
    (tmp/'authority.inc').write_text('\n'.join(extract(buffer,sig) for sig in (
        'void Buffer::VisitPhysicalRanges(', 'void Buffer::TrackGuestUsage(', 'void Buffer::ClearSparseHoles(', 'bool Buffer::HasSparseHoles(', 'bool Buffer::HasRepeatedPhysicalMappings(',
        'void Buffer::unlock(', 'bool Buffer::Write(', 'bool Buffer::WriteImplStaged(', 'void Buffer::WriteImmediateRepeated(', 'void Buffer::CopyFrom(', 'void Buffer::SynchronizeHost(', 'bool Buffer::SynchronizeGuest(', 'void Buffer::MarkGpuDirty('))+'\n'+
        '\n'.join(extract(manager,sig) for sig in ('void BufferMappingReconciliationRequest::Complete(',
        'std::vector<std::shared_ptr<Buffer>> BufferManager::SnapshotAliases(', 'static bool PhysicalRangesOverlap(',
        'void BufferManager::PrepareBufferAccess(', 'void BufferManager::NotifyBufferWrite(', 'BufferView BufferManager::FindOrCreateDisjoint(', 'BufferManager::LockedBuffer BufferManager::CoalesceBuffers(')))
    interconnect = ROOT/'app/src/main/cpp/skyline/gpu/interconnect'
    (tmp/'boundaries.inc').write_text('\n'.join(
        extract((interconnect/path).read_text(), signature)
        for path, signature in (
            ('inline2memory.cpp', 'void Inline2Memory::UploadSingleMapping('),
            ('maxwell_dma.cpp', 'void MaxwellDma::Copy('),
            ('maxwell_dma.cpp', 'void MaxwellDma::Clear('),
            ('maxwell_3d/maxwell_3d.cpp', 'void Maxwell3D::LoadConstantBuffer('),
            ('maxwell_3d/maxwell_3d.cpp', 'void Maxwell3D::BindConstantBuffer('))))
    for test in ('tests','authority','boundaries'):
        binary = tmp/test
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(tmp),str(ROOT/('tests/gpu_disjoint_buffers/'+test+'.cpp')),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
