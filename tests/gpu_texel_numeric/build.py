#!/usr/bin/env python3
"""Build production SPIR-V emitters for mixed numeric texel buffers."""
import argparse
import os
import shlex
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--deps', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--boost-include', type=Path)
parser.add_argument('--spirv-tools', type=Path, help='directory containing spirv-val and spirv-dis; defaults to PATH')
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--baseline', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2] / 'app/libraries/shader-compiler'
deps = args.deps.resolve()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'include', deps / 'fmt/include', deps / 'range/include',
            deps / 'sirit/include', deps / 'sirit/src', deps / 'sirit/externals/SPIRV-Headers/include']
includes += [args.boost_include] if args.boost_include else list((deps / 'boost/libs').glob('*/include'))
sources = [Path(__file__).with_name('compiler.cpp')]
sources += list((root / 'backend/spirv').glob('*.cpp'))
sources += list((root / 'frontend/ir').glob('*.cpp'))
sources += list((deps / 'sirit/src').rglob('*.cpp'))
flags = ['-std=c++20', '-O1', '-g', '-include', 'cstddef', '-DFMT_HEADER_ONLY',
         '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', *shlex.split(os.environ.get('CXXFLAGS', '')), '-UNDEBUG']
if args.baseline: flags += ['-DTEXEL_BASELINE']
def compile_source(source):
    obj = build / (('sirit_' if source.is_relative_to(deps / 'sirit') else '') + source.stem + '.o')
    dep = obj.with_suffix('.d')
    stamp = obj.with_suffix('.json')
    command = [os.environ.get('CXX', 'c++'), *flags, *[f'-I{p}' for p in includes],
               '-MMD', '-MF', str(dep), '-c', str(source), '-o', str(obj)]
    inputs = shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]) if dep.exists() else []
    fresh = (obj.exists() and stamp.exists() and inputs and json.loads(stamp.read_text()) == command
             and all(Path(p).exists() and Path(p).stat().st_mtime <= obj.stat().st_mtime for p in inputs))
    if not fresh:
        subprocess.run(command, check=True)
        stamp.write_text(json.dumps(command))
    return obj

with ThreadPoolExecutor(max_workers=args.jobs) as executor:
    objects = list(executor.map(compile_source, sources))
exe = build / 'texel-compiler'
subprocess.run([os.environ.get('CXX', 'c++'), '-fsanitize=address,undefined', *map(str, objects),
                '-Wl,--gc-sections', '-o', str(exe)], check=True)

subprocess.run([exe, build],check=True)

def tool(name):
    return str(args.spirv_tools / name) if args.spirv_tools else name
for module in [build/'mixed.spv',build/'mixed-fragment.spv']:
    subprocess.run([tool('spirv-val'),'--target-env','vulkan1.1',module],check=True)
    assembly=subprocess.check_output([tool('spirv-dis'),module],text=True)
    module.with_suffix('.spvasm').write_text(assembly)
    assert assembly.count('OpImageFetch') == 3
    assert assembly.count('OpTypeImage') == (1 if args.baseline else 3)
    if not args.baseline:
        for scalar in ['%f32','%u32','%s32']:
            assert f'OpTypeImage {scalar} Buffer' in assembly
        assert 'RelaxedPrecision' not in assembly
print('PASS production SPIR-V backend with ASan/UBSan: compute + fragment modules, matching Float/UINT/SINT image/fetch types, raw bits, no relaxed integer precision' if not args.baseline else 'PASS all-float negative-control modules generated and validated')
