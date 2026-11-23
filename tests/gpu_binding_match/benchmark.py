#!/usr/bin/env python3
"""Paired CPU microbenchmark of production compatibility checks and draw gate.

Extracts the full CheckBindingMatch function and descriptor-update lambda from
--baseline and the working tree. Uses production PipelineSpecialization and
pinned robin_map; resource/descriptor operations are host facades. No Vulkan,
Android, FPS or full PrepareDraw measurement is implied.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'
PM = 'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp'
DRAW = 'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.cpp'


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, help='Git ref before optimization')
    parser.add_argument('--iterations', type=int, default=500000)
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.iterations < 1 or args.rounds < 3:
        parser.error('positive iterations and at least three rounds required')
    baseline = subprocess.check_output(['git', 'rev-parse', args.baseline], cwd=ROOT, text=True).strip()
    robin = ROOT / 'app/libraries/robin-map/include'
    if not (robin / 'tsl/robin_map.h').exists():
        raise SystemExit('Initialize the pinned robin-map submodule first')
    cpu = min(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpu})
    cxx = os.environ.get('CXX', 'g++')
    with tempfile.TemporaryDirectory(prefix='strato-binding-bench-') as tmp:
        build = Path(tmp)
        fixture = (Path(__file__).with_name('fixture.inc')).read_text()
        pieces = ['#include "benchmark.h"\n']
        for name in ('before', 'after'):
            def read(path):
                if name == 'after':
                    return (ROOT / path).read_text()
                return subprocess.check_output(['git', 'show', f'{baseline}:{path}'], cwd=ROOT, text=True)
            match = function(read(PM), 'bool Pipeline::CheckBindingMatch(')
            gate = function(read(DRAW), '[&]() -> DescriptorUpdateInfo *')
            pieces += [f'namespace {name} {{\n', fixture, '\n__attribute__((noinline)) ' + match,
                       '\n__attribute__((noinline)) DescriptorUpdateInfo *Prepare(Pipeline *activeDescriptorPipeline, Pipeline *pipeline, bool enabled) {\n',
                       'Context ctx; int samplers{}, textures{}, activeDescriptorSetSampledImages{}, srcStageMask{}, dstStageMask{};\n',
                       'struct { bool quickBindEnabled; std::optional<int> quickBind; int boundConstantBuffers; } constantBuffers{enabled,0,0};\n',
                       'auto prepare = ' + gate + '; return prepare();\n}\n}\n']
        pieces += [(Path(__file__).with_name('benchmark.cpp')).read_text()]
        source = build / 'benchmark.cpp'
        source.write_text(''.join(pieces))
        binary = build / 'benchmark'
        subprocess.run([cxx, '-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(CPP), '-I' + str(robin), '-I' + str(Path(__file__).parent),
                        str(source), '-o', str(binary)], check=True)
        raw = subprocess.check_output([str(binary), str(args.iterations), str(args.rounds)], text=True)
    results = json.loads(raw)
    for row in results:
        row['before_median_ns'] = statistics.median(row['before_ns'])
        row['after_median_ns'] = statistics.median(row['after_ns'])
        row['speedup'] = row['before_median_ns'] / row['after_median_ns']
    report = {'baseline': baseline, 'host': platform.platform(), 'cpu': cpu,
              'compiler': subprocess.check_output([cxx, '--version'], text=True).splitlines()[0],
              'flags': '-std=c++20 -O3 -DNDEBUG (no LTO)', 'iterations_per_round': args.iterations,
              'rounds': args.rounds, 'results': results}
    output = json.dumps(report, indent=2) + '\n'
    print(output, end='')
    if args.output:
        args.output.write_text(output)


if __name__ == '__main__':
    main()
