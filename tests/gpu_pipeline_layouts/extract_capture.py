#!/usr/bin/env python3
"""Strict offline reader of layout creation chunks in a RenderDoc Vulkan v32 RDC.

Only layout metadata is exported: no shaders, resources or game bytes. Unknown
extensions/immutable samplers/versions fail closed rather than guessing layouts.
Format references (RenderDoc, MIT):
https://github.com/baldurk/renderdoc/tree/5ad608809c9028da9f5c5082397cb6afe9c8dd17/renderdoc/serialise
https://github.com/baldurk/renderdoc/blob/5ad608809c9028da9f5c5082397cb6afe9c8dd17/renderdoc/driver/vulkan/vk_serialise.cpp
"""
import argparse
import collections
import ctypes
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser()
p.add_argument('capture', type=Path)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
raw = args.capture.read_bytes()
assert raw[:8] == b'RDOC\0\0\0\0'
offset = struct.unpack_from('<I', raw, 12)[0]
sections = []
while offset < len(raw):
    header = struct.unpack_from('<IIQQQII', raw, offset)
    assert header[0] == 0 and 0 < header[6] < 2048
    start = offset + 40 + header[6]
    end = start + header[2]
    assert end <= len(raw)
    if header[1] == 1:
        sections.append((header, start, end))
    offset = end
assert offset == len(raw) and len(sections) == 1
header, offset, end = sections[0]
assert header[4] == 32 and header[5] == 2  # Vulkan serialization v32, LZ4 blocks
lz4 = ctypes.CDLL('liblz4.so.1').LZ4_decompress_safe_usingDict
lz4.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_void_p, ctypes.c_int]
lz4.restype = ctypes.c_int
data = bytearray(header[3])
written = 0
history = b''
while offset < end:
    length = struct.unpack_from('<i', raw, offset)[0]
    offset += 4
    assert 0 < length < 1100000 and offset + length <= end
    source = ctypes.create_string_buffer(raw[offset:offset + length])
    destination = ctypes.create_string_buffer(1024 * 1024)
    dictionary = ctypes.create_string_buffer(history)
    size = lz4(source, destination, length, len(destination), dictionary, len(history))
    assert 0 < size <= len(destination) and written + size <= len(data)
    part = destination.raw[:size]
    data[written:written + size] = part
    written += size
    history = part[-65536:]
    offset += length
assert written == len(data) and offset == end

chunks = []
offset = 0
while offset < len(data):
    tag = struct.unpack_from('<I', data, offset)[0]
    offset += 4
    assert tag & 0xffff and not tag & ~0x1fffff
    if tag & 0x10000:
        frames = struct.unpack_from('<I', data, offset)[0]
        offset += 4 + frames * 8
    for flag in (0x20000, 0x40000, 0x80000):
        if tag & flag:
            offset += 8
    fmt = '<Q' if tag & 0x100000 else '<I'
    length = struct.unpack_from(fmt, data, offset)[0]
    offset += struct.calcsize(fmt)
    assert offset + length <= len(data)
    chunks.append((tag & 0xffff, offset, length))
    offset = (offset + length + 63) // 64 * 64
assert len(data) <= offset < len(data) + 64

sets = {}
layouts = []
for index, (kind, start, length) in enumerate(chunks):
    if kind not in (1012, 1020):  # vkCreateDescriptorSetLayout / vkCreatePipelineLayout
        continue
    payload = memoryview(data)[start:start + length]
    cursor = 0

    def read(fmt):
        global cursor
        values = struct.unpack_from('<' + fmt, payload, cursor)
        cursor += struct.calcsize('<' + fmt)
        return values[0] if len(values) == 1 else values

    device = read('Q')
    assert read('I') == {1012: 32, 1020: 30}[kind]  # VkStructureType
    assert not read('?')  # pNext is absent
    flags = read('I')
    count = read('I')
    assert count == read('Q')
    if kind == 1012:
        bindings = []
        for _ in range(count):
            bindings.append(read('IIII'))
            assert read('Q') == 0  # immutable sampler array is absent
        assert not read('?')  # allocation callbacks absent
        resource = read('Q')
        sets[resource] = {'flags': flags, 'bindings': bindings}
    else:
        handles = [read('Q') for _ in range(count)]
        nranges = read('I')
        assert nranges == read('Q')
        ranges = [read('III') for _ in range(nranges)]
        assert not read('?')
        resource = read('Q')
        assert len(handles) == 1  # current Strato graphics/compute builders
        layouts.append({'resource_id': resource, 'chunk': index, 'flags': flags,
                        'descriptor': sets[handles[0]], 'ranges': ranges})
    assert cursor == length  # consuming exactly the chunk verifies the schema

key = lambda item: json.dumps([item['flags'], item['descriptor'], item['ranges']], sort_keys=True)
summary = {'capture_name': args.capture.name, 'capture_sha256': hashlib.sha256(raw).hexdigest(),
           'capture_bytes': len(raw), 'uncompressed_bytes': len(data), 'chunks': len(chunks),
           'descriptor_creates': len(sets), 'pipeline_layout_creates': len(layouts),
           'unique_descriptors': len({json.dumps(s, sort_keys=True) for s in sets.values()}),
           'unique_pipeline_layouts': len({key(i) for i in layouts}),
           'repeated_group_sizes': [n for n in collections.Counter(key(i) for i in layouts).values() if n > 1]}
args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'capture-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
with (args.output / 'capture-layouts.txt').open('w') as output:
    for item in layouts:
        bindings = item['descriptor']['bindings']
        ranges = item['ranges']
        row = [item['descriptor']['flags'], item['flags'], len(bindings), len(ranges)]
        row += [v for binding in bindings for v in binding]
        row += [v for value in ranges for v in value]
        output.write(' '.join(map(str, row)) + '\n')
print(json.dumps(summary))
