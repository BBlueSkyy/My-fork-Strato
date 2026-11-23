#!/usr/bin/env python3
"""Native-only layout validation or optional paired Vulkan microbenchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
p = argparse.ArgumentParser()
p.add_argument('--benchmark', action='store_true')
p.add_argument('--build-dir', type=Path)
p.add_argument('--fixture', type=Path, default=HERE / 'capture-layouts.txt')
p.add_argument('--unique-control', action='store_true')
p.add_argument('--suite', action='store_true', help='capture plus repeated/unique/mixed/variant/growth controls')
p.add_argument('--review-ref', help='compare the initial PR cache from this local git ref')
args = p.parse_args()
if args.review_ref and not args.benchmark:
    p.error('--review-ref requires --benchmark')
if args.benchmark and hasattr(os, 'sched_getaffinity'):
    os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
directory = (args.build_dir or Path(tempfile.mkdtemp(prefix='strato-layouts-'))).resolve()
directory.mkdir(parents=True, exist_ok=True)
if args.suite and (not args.benchmark or args.unique_control):
    p.error('--suite requires --benchmark and cannot combine with --unique-control')
if args.unique_control or args.suite:
    if not args.benchmark:
        p.error('--unique-control requires --benchmark')
    from make_workloads import make_workloads
    workloads = make_workloads(directory / 'workloads')
    if args.unique_control:
        args.fixture = workloads['unique']
if args.review_ref:
    original = subprocess.check_output(['git', 'show', args.review_ref + ':app/src/main/cpp/skyline/gpu/pipeline_layout_cache.h'], cwd=ROOT, text=True)
    print(json.dumps({'reference_cache_sha256': hashlib.sha256(original.encode()).hexdigest()}), flush=True)
    # Visibility changes only expose the existing hash/key helpers for isolated
    # diagnostics. No reference algorithm or data member changes on this path.
    (directory / 'previous-layout-cache.h').write_text(original.replace('namespace skyline::gpu {', 'namespace previous_cache {').replace('      private:', '      public:'))
    (directory / 'unlocked-layout-cache.h').write_text(original.replace('namespace skyline::gpu {', 'namespace unlocked_cache {').replace('std::mutex mutex;', 'struct { void lock() {} void unlock() {} } mutex;'))

if not args.review_ref:
    for name in ('previous-layout-cache.h', 'unlocked-layout-cache.h'):
        (directory / name).unlink(missing_ok=True)

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
    # Compile the actual helper handle consumers too: these were missed by the
    # original integration harness. Only platform state/push-data types are facades.
    import re
    helper = (gpu / 'shaders/helper_shaders.cpp').read_text()
    allocation = re.findall(r'descriptorSet\{(gpu\.descriptor\.AllocateSet\([^\n]+?\))\},', helper)
    commands = re.findall(r'commandBuffer\.(?:bindDescriptorSets|pushConstants)\([^;]*drawState->pipeline\.pipelineLayout[^;]*;', helper)
    if len(allocation) != 1 or len(commands) != 4:
        raise RuntimeError('helper layout consumers changed; update the compile fixture')
    body = """namespace helper_consumers {
    using namespace skyline::gpu;
    struct AllocatorFacade { DescriptorAllocator &descriptor; };
    [[maybe_unused]] inline auto Allocate(AllocatorFacade &gpu, const GraphicsPipelineAssembler::CompiledPipeline &pipeline) {
        return ALLOCATION;
    }
    namespace blit { struct VertexPushConstantLayout { float v[4]; }; struct FragmentPushConstantLayout { float v[4]; }; }
    namespace clear { struct FragmentPushConstantLayout { float color[4]; uint32_t clearDepth; float depth; }; }
    [[maybe_unused]] inline void Blit(vk::raii::CommandBuffer &commandBuffer,
                       const GraphicsPipelineAssembler::CompiledPipeline &pipeline, DescriptorAllocator::ActiveDescriptorSet &descriptorSet) {
        struct State { const GraphicsPipelineAssembler::CompiledPipeline &pipeline; DescriptorAllocator::ActiveDescriptorSet &descriptorSet;
            blit::VertexPushConstantLayout vertexPushConstants; blit::FragmentPushConstantLayout fragmentPushConstants; } state{pipeline, descriptorSet, {}, {}};
        auto *drawState=&state;
        BLIT_COMMANDS
    }
    [[maybe_unused]] inline void Clear(vk::raii::CommandBuffer &commandBuffer,
                       const GraphicsPipelineAssembler::CompiledPipeline &pipeline) {
        struct State { const GraphicsPipelineAssembler::CompiledPipeline &pipeline; clear::FragmentPushConstantLayout fragmentPushConstants; } state{pipeline, {}};
        auto *drawState=&state;
        CLEAR_COMMAND
    }
    }
    """
    body = body.replace('ALLOCATION', allocation[0]).replace('BLIT_COMMANDS', '\n'.join(commands[:3])).replace('CLEAR_COMMAND', commands[3])
    (directory / 'helper-calls.inc').write_text(body)
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
            ROOT / 'app/libraries/robin-map/include',
            Path(os.environ.get('VULKAN_HPP_INCLUDE_DIR', ROOT / 'app/libraries/vkhpp')),
            Path(os.environ.get('VULKAN_HEADERS_INCLUDE_DIR', ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))]
command = [compiler, *flags, '-I' + str(directory), '-I' + str(includes[0]), *[x for i in includes[1:] for x in ('-isystem', str(i))],
           *[str(HERE / s) for s in sources], os.environ.get('VULKAN_LOADER_LIBRARY', '-lvulkan'), '-ldl']
subprocess.run([*command, '-o', str(binary)], check=True)
if args.benchmark:
    diagnostic = directory / 'diagnostic'
    subprocess.run([*command, '-DLAYOUT_DIAGNOSTICS', '-o', str(diagnostic)], check=True)
    cases = {'capture': HERE / 'capture-layouts.txt', **workloads} if args.suite else {'fixture': args.fixture}
    for name, fixture in cases.items():
        print(json.dumps({'kind': 'workload', 'name': name, 'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest()}), flush=True)
        subprocess.run([str(binary), str(fixture)], check=True)
        subprocess.run([str(diagnostic), str(fixture)], check=True)
else:
    subprocess.run([str(binary), str(directory)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': os.environ.get('ASAN_OPTIONS', 'detect_leaks=0')})
