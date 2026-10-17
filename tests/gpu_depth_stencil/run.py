#!/usr/bin/env python3
"""Exercise production aspect regions and packed format metadata on the host."""
import argparse
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--ref', help='Negative control: use region generation and metadata from this revision')
parser.add_argument('--benchmark', action='store_true', help='Unsanitized CPU microbenchmark; not FPS')
args = parser.parse_args()

def read(path):
    return (subprocess.check_output(['git', 'show', args.ref + ':' + path], cwd=ROOT, text=True)
            if args.ref else (ROOT / path).read_text())

source = read('app/src/main/cpp/skyline/gpu/texture/texture.cpp')
signature = 'boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies('
start = source.index(signature)
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
formats = read('app/src/main/cpp/skyline/gpu/texture/format.h')
formats = formats[formats.index('// Depth/Stencil Formats'):formats.index('    #undef FORMAT')]
with tempfile.TemporaryDirectory(prefix='strato-depth-stencil-') as directory:
    tmp = pathlib.Path(directory)
    (tmp / 'regions.inc').write_text(source[start:end])
    (tmp / 'formats.inc').write_text(formats)
    includes = [ROOT / 'app/src/main/cpp/skyline', ROOT / 'app/libraries/vkhpp',
                pathlib.Path(os.environ.get('VULKAN_HEADERS_INCLUDE_DIR', str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'))), tmp]
    flags = ['-O2'] if args.benchmark else ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', *flags, *['-I' + str(p) for p in includes],
                    str(ROOT / 'tests/gpu_depth_stencil/tests.cpp'), '-o', str(tmp / 'tests')], check=True)
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run([str(tmp / 'tests')] + (['--benchmark'] if args.benchmark else []), env=env, check=True)
