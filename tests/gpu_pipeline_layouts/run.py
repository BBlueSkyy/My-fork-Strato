#!/usr/bin/env python3
"""Native-only layout validation or optional paired Vulkan microbenchmark."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
p = argparse.ArgumentParser()
p.add_argument('--benchmark', action='store_true')
p.add_argument('--build-dir', type=Path)
p.add_argument('--fixture', type=Path, default=HERE / 'capture-layouts.txt')
p.add_argument('--unique-control', action='store_true')
args = p.parse_args()
if args.benchmark and hasattr(os, 'sched_getaffinity'):
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
directory = args.build_dir or Path(tempfile.mkdtemp(prefix='strato-layouts-'))
directory.mkdir(parents=True, exist_ok=True)
if args.unique_control:
    if not args.benchmark:
        p.error('--unique-control requires --benchmark')
    args.fixture = directory / 'unique-control.txt'
    with args.fixture.open('w') as fixture:
        for i in range(128):
            stage = 1 if i & 64 else 16
            fixture.write(f'0 0 3 0 0 6 {1 + i % 4} {stage} '
                          f'1 7 {1 + (i // 4) % 4} {stage} '
                          f'2 1 {1 + (i // 16) % 4} {stage}\n')
def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

if not args.benchmark:
    gpu = ROOT / 'app/src/main/cpp/skyline/gpu'
    header = (gpu / 'descriptor_allocator.h').read_text()
    source = (gpu / 'descriptor_allocator.cpp').read_text()
    strip_includes = lambda text: '\n'.join(x for x in text.splitlines() if not x.startswith('#include'))
    (directory / 'descriptor-header.inc').write_text(strip_includes(header))
    (directory / 'descriptor-source.inc').write_text(strip_includes(source))
    graphics = (gpu / 'graphics_pipeline_assembler.cpp').read_text()
    signatures = ['GraphicsPipelineAssembler::PipelineDescription::PipelineDescription(',
                  'vk::raii::Pipeline GraphicsPipelineAssembler::AssemblePipeline(',
                  'GraphicsPipelineAssembler::CompiledPipeline GraphicsPipelineAssembler::AssemblePipelineAsync(',
                  'void GraphicsPipelineAssembler::WaitIdle()']
    body = '#define VEC_CPY(pointer, size) state.pointer, state.pointer + state.size\n'
    body += '\n'.join(extract(graphics, x) for x in signatures)
    (directory / 'graphics-source.inc').write_text('namespace skyline::gpu {\n' + body + '\n#undef VEC_CPY\n}\n')
    compute = gpu / 'interconnect/kepler_compute/pipeline_manager'
    (directory / 'compute-compiled.inc').write_text(extract(compute.with_suffix('.h').read_text(), 'struct CompiledPipeline') + ';\n')
    (directory / 'compute-source.inc').write_text(extract(compute.with_suffix('.cpp').read_text(), 'static Pipeline::CompiledPipeline MakeCompiledPipeline('))
    assembler = shutil.which('spirv-as')
    validator = shutil.which('spirv-val')
    if not assembler or not validator:
        raise SystemExit('spirv-as and spirv-val are required')
    for name in ('compute', 'compute3', 'vertex', 'fragment'):
        source = (HERE / (('compute' if name == 'compute3' else name) + '.spvasm')).read_text()
        if name == 'compute3':
            source = source.replace('%factor = OpConstant %uint 1', '%factor = OpConstant %uint 3')
        asm = directory / (name + '.spvasm')
        asm.write_text(source)
        binary_shader = directory / (name + '.spv')
        subprocess.run([assembler, '--target-env', 'vulkan1.1', str(asm), '-o', str(binary_shader)], check=True)
        subprocess.run([validator, '--target-env', 'vulkan1.1', str(binary_shader)], check=True)

compiler = os.environ.get('CXX') or shutil.which('clang++') or 'g++'
sources = ['benchmark.cpp'] if args.benchmark else ['tests.cpp']
binary = directory / ('benchmark' if args.benchmark else 'tests')
flags = ['-std=c++20', '-pthread', '-g', '-O3' if args.benchmark else '-O1',
         '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
         '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-DVULKAN_HPP_NO_SPACESHIP_OPERATOR',
         '-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0']
if 'clang' in Path(compiler).name:
    flags += ['-Wno-reorder-init-list', '-Wno-unused-private-field']
if not args.benchmark:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
else:
    flags += ['-DNDEBUG']
includes = [ROOT / 'app/src/main/cpp/skyline', ROOT / 'app/libraries/thread-pool', Path(os.environ.get('BOOST_INCLUDE_DIR', ROOT / 'app/libraries/boost')),
            Path(os.environ.get('VULKAN_HPP_INCLUDE_DIR', ROOT / 'app/libraries/vkhpp')),
            Path(os.environ.get('VULKAN_HEADERS_INCLUDE_DIR', ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))]
subprocess.run([compiler, *flags, '-I' + str(directory), '-I' + str(includes[0]), *[x for i in includes[1:] for x in ('-isystem', str(i))],
                *[str(HERE / s) for s in sources], os.environ.get('VULKAN_LOADER_LIBRARY', '-lvulkan'),
                '-ldl', '-o', str(binary)], check=True)
subprocess.run([str(binary), *([str(args.fixture)] if args.benchmark else [str(directory)])], check=True,
               env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
