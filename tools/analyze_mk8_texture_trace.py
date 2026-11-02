#!/usr/bin/env python3
"""Summarize bounded MK8 traces without interpreting missing events as proof."""
import argparse
import collections
import csv
from pathlib import Path


def analyze(path):
    lines = Path(path).read_text().splitlines()
    for line in lines:
        if line.startswith('# phase='):
            print(line)
    rows = list(csv.DictReader(line for line in lines if not line.startswith('#')))
    labels = ['startup', 'first-selection', 'race', 'returned-selection']
    for phase, label in enumerate(labels):
        current = [r for r in rows if int(r['phase']) == phase]
        events = collections.Counter(r['event'] for r in current)
        reads = {r['id'] for r in current if r['event'] in ('SampleRead', 'StorageRead')}
        candidates = {r['id'] for r in current if r['event'] in ('InlineGpuOwnership', 'HostGpuOwnership')
                      and int(r['write_serial']) == 0 and r['id'] in reads}
        freed = {r['id'] for r in current if r['event'] == 'FreeGuest'}
        print(f'{phase} {label}: rows={len(current)} textures={len({r["id"] for r in current})} events={dict(events)}')
        print(f'  read-bound ownership with zero write serial: {", ".join(sorted(candidates, key=int)[:40]) or "none recorded"}')
        print(f'  same IDs with observed FreeGuest: {", ".join(sorted(candidates & freed, key=int)[:40]) or "none recorded"}')
    print('Candidate only: zero write serial means no RecordGpuWrite before this snapshot; it does not prove image corruption.')
    print('Compare IDs, mapping intervals, aliases and subresources across phases. Address reuse is not texture identity.')
    print('Counters describe missing/limited evidence. Bindings/writer intent do not prove conditional GPU instructions executed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace')
    analyze(parser.parse_args().trace)
