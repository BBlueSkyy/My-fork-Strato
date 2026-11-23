#!/usr/bin/env python3
"""Summarize paired review runs and separate no-tcache heap diagnostics."""
import json
from pathlib import Path
import statistics
import sys


def records(path):
    cases = {}
    current = None
    for line in Path(path).read_text().splitlines():
        row = json.loads(line)
        if row.get('kind') == 'workload':
            current = row['name']
            cases[current] = []
        elif current:
            cases[current].append(row)
    return cases


def summarize(rounds, heap):
    memory = records(heap)
    result = {}
    for name, rows in records(rounds).items():
        result[name] = {}
        for variant in ('master', 'previous', 'revised', 'previous_no_lock'):
            timing = [r for r in rows if r.get('kind') == 'timing' and r['variant'] == variant]
            if len(timing) != 12:
                raise ValueError(f'incomplete timing: {name}/{variant}')
            elapsed = [r['total_creation_ns'] / r['epochs'] for r in timing]
            stats = dict(creation_median_ns=statistics.median(elapsed),
                         creation_min_ns=min(elapsed), creation_max_ns=max(elapsed),
                         lifecycle_median_ns=statistics.median(r['total_lifecycle_ns'] / r['epochs'] for r in timing))
            objects = [r for r in memory[name] if r.get('kind') == 'objects' and r['variant'] == variant]
            if objects:
                if len(objects) != 5:
                    raise ValueError(f'incomplete diagnostics: {name}/{variant}')
                stats.update({k: objects[0][k] for k in ('requests', 'descriptor_creates', 'pipeline_layout_creates',
                                                       'pipeline_layout_cache_hits', 'descriptor_reuses', 'cpp_new_calls', 'cpp_new_bytes')})
                stats['heap_delta_median_bytes_no_tcache'] = statistics.median(r['heap_delta_bytes'] for r in objects)
            result[name][variant] = stats
        result[name]['isolated_costs_ns'] = {
            stage: statistics.median(r['total_ns'] / r['epochs'] for r in rows if r.get('kind') == 'isolated' and r['stage'] == index)
            for index, stage in enumerate(('descriptor_hash', 'pipeline_hash', 'owned_descriptor_keys', 'uncontended_mutex'))}
    return result


if __name__ == '__main__':
    print(json.dumps(summarize(*sys.argv[1:]), indent=2))
