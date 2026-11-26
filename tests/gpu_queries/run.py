#!/usr/bin/env python3
"""Compile unchanged production Queries and resolve recording against host interfaces."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix='strato-gpu-queries-') as directory:
    directory = pathlib.Path(directory)
    header = (GPU / 'interconnect/maxwell_3d/queries.h').read_text()
    source = (GPU / 'interconnect/maxwell_3d/queries.cpp').read_text()
    # Only platform includes are replaced. Classes, members and method bodies remain unchanged.
    (directory / 'queries_header.inc').write_text(header[header.index('namespace skyline'):])
    (directory / 'queries_source.inc').write_text(source[source.index('namespace skyline'):])
    shader = (ROOT / 'app/src/main/shaders/query_resolve.comp').read_text()
    (directory / 'shader.inc').write_text('\n'.join(function(shader, signature) for signature in (
        'uvec2 add64(', 'void main(')))
    helper = (GPU / 'shaders/helper_shaders.cpp').read_text()
    (directory / 'resolve.inc').write_text('\n'.join(function(helper, signature) for signature in (
        'void QueryResolveHelperShader::Resolve(',
        'void QueryResolveHelperShader::PrepareForTransfer(')))
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I' + str(directory),
                    str(ROOT / 'tests/gpu_queries/tests.cpp'), '-o', str(directory / 'tests')], check=True)
    environment = dict(os.environ)
    # LeakSanitizer cannot enumerate threads under this runner's ptrace sandbox.
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run([str(directory / 'tests')], check=True, env=environment)
