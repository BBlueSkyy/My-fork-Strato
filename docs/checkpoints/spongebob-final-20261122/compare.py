#!/usr/bin/env python3
"""Replay supplied evidence only; no emulator/GPU work or diagnostic hooks."""
import collections
import hashlib
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent

def run():
    sessions = []
    for name in ('boot1.events.json', 'boot2.events.json'):
        events = json.loads((HERE / name).read_text())
        assert events[0]['free_guest'] == 0
        bykind = collections.defaultdict(list)
        for event in events:
            bykind[event['kind']].append(event)
        texels = bykind['growth-texel']
        assert len(texels) == 12
        assert all(e['detail'][0] == e['detail'][1] and e['detail'][2] == 1 for e in texels)
        assert [e['view'][:2] for e in texels] == [[1, i] for i in range(6)] + [[i, 0] for i in range(2, 8)]
        base = bykind['growth-begin'][0]['maps'][0][0]
        for e in texels:
            assert e['a'] == base + e['b']
        writers = bykind['growth-writer']
        assert len(writers) == 7
        assert all(e['dirty'] == 2 and e['readback'] == 0 and e['detail'][:2] == [1, 0] for e in writers)
        consumer = bykind['consumer-added-levels'][0]
        assert consumer['view'] == [1, 0, 6] and consumer['a'] == 1
        assert consumer['destinations'] == [[57, 97, 32, 32, 0, 0]]
        assert consumer['detail'][6] == 1  # sampler association ambiguous
        assert not bykind['readback-published']
        sessions.append(dict(
            events=len(events), kinds=dict(collections.Counter(e['kind'] for e in events)),
            hdr_prefix={k:bykind['upload-prepared'][0][k] for k in ('nan','excess','samples')},
            guest_hash=bykind['upload-source'][0]['hash'],
            texels=[dict(mip=e['view'][0],layer=e['view'][1],guest_offset=e['b'],staging_offset=e['c'],
                         bits=f"{e['detail'][0]:016x}",
                         rgba=[str(x) for x in struct.unpack('<4e',e['detail'][0].to_bytes(8,'little'))]) for e in texels],
            writer_serials=[e['write'] for e in writers],
            consumer_shaders=[f'{x:016x}' for x in consumer['shaders'] if x],
            sampler_limits=[struct.unpack('<f',x.to_bytes(4,'little'))[0] for x in consumer['detail'][1:4]],
            last={k:events[-1].get(k) for k in ('seq','lost','image_limit')},
        ))
    a,b=[json.loads((HERE / n).read_text()) for n in ('boot1.events.json','boot2.events.json')]
    assert [e['kind'] for e in a[:52]] == [e['kind'] for e in b[:52]]
    assert {x['write']-y['write'] for x,y in zip(a,b) if x.get('write') and y.get('write')} == {525}
    assert sum(x['bits'] != y['bits'] for x,y in zip(sessions[0]['texels'],sessions[1]['texels'])) == 12
    return dict(sessions=sessions,matched_prefix_records=52,serial_translation=525,
                different_corresponding_texels=12,causal_visual_violation_proven=False,
                decision='close_without_renderer_patch_or_further_device_request')

if __name__ == '__main__':
    print(json.dumps(run(),indent=2))
