#!/usr/bin/env python3
"""Run pinned production TexturePass; --deps supplies initialized host headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
parser = argparse.ArgumentParser()
parser.add_argument('--deps', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
compiler = root / 'app/libraries/shader-compiler'
deps = args.deps.resolve()
includes = [compiler, compiler / 'include', deps / 'fmt/include', deps / 'range/include']
includes += list((deps / 'boost/libs').glob('*/include'))
with tempfile.TemporaryDirectory(prefix='strato-tic-shader-') as directory:
    binary = Path(directory) / 'tests'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-w', '-include', 'cstddef',
        '-DFMT_HEADER_ONLY', '-ffunction-sections', '-fdata-sections', '-fsanitize=undefined', '-fno-sanitize-recover=all',
        *[f'-I{p}' for p in includes], str(Path(__file__).with_name('shader.cpp')),
        str(compiler / 'ir_opt/texture_pass.cpp'), *map(str, (compiler / 'frontend/ir').glob('*.cpp')),
        '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
