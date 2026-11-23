#!/usr/bin/env python3
"""Deterministic valid, dense layout-only controls; no game data."""
from pathlib import Path
import sys


def make_workloads(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    unique = []
    for i in range(128):
        stage = 1 if i & 64 else 16
        unique.append(f'0 0 3 0 0 6 {1 + i % 4} {stage} '
                      f'1 7 {1 + (i // 4) % 4} {stage} '
                      f'2 1 {1 + (i // 16) % 4} {stage}')
    rows = {'unique': unique,
            'repeated': [unique[i % 8] for i in range(128)],
            'mixed': unique[:64] + [unique[(i * 17) % 64] for i in range(64)]}
    rows['push_variants'] = []
    for i in range(128):
        row = unique[i % 8].split()
        rows['push_variants'].append(' '.join([*row[:3], '1', *row[4:], '16', str((i // 8) * 4), '4']))
    rows['growth'] = []
    for i in range(2048):
        stage = [1, 16, 32, 17][i // 512]
        rows['growth'].append(f'0 0 3 0 0 6 {1 + i % 8} {stage} '
                              f'1 7 {1 + (i // 8) % 8} {stage} '
                              f'2 1 {1 + (i // 64) % 8} {stage}')
    paths = {}
    for name, definitions in rows.items():
        paths[name] = directory / (name + '.txt')
        paths[name].write_text('\n'.join(definitions) + '\n')
    return paths


if __name__ == '__main__':
    make_workloads(sys.argv[1])
