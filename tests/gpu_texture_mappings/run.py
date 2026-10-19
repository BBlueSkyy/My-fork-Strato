#!/usr/bin/env python3
"""Exercise complete production TIC lookup/cache methods with real GMMU/mirrors."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'


def extract(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix='strato-texture-mappings-') as directory:
    tmp = pathlib.Path(directory)
    for name in ('address_space.h', 'address_space.inc', 'segment_table.h', 'dirty_tracking.h'):
        source = (CPP / 'common' / name).read_text()
        (tmp / name).write_text('\n'.join(line for line in source.splitlines() if not line.startswith('#include')))
    tic = (CPP / 'gpu/interconnect/common/tic.h').read_text()
    (tmp / 'tic.inc').write_text('\n'.join(line for line in tic.splitlines() if not line.startswith('#include')))
    header = (CPP / 'gpu/interconnect/common/textures.h').read_text()
    source = (CPP / 'gpu/interconnect/common/textures.cpp').read_text()
    baseline = os.environ.get('TEXTURE_MAPPINGS_BASELINE_REF')
    if baseline:
        path = 'app/src/main/cpp/skyline/gpu/interconnect/common/textures.cpp'
        value = subprocess.check_output(['git', 'show', baseline + ':' + path], cwd=ROOT, text=True)
        signature = 'TextureView *Textures::GetTexture('
        source = source.replace(extract(source, signature), extract(value, signature))
    classes = '\n'.join(extract(header, 'class ' + name) + ';' for name in ('TexturePoolState', 'Textures'))
    (tmp / 'classes.inc').write_text(classes)
    signatures = ('void TexturePoolState::EngineRegisters::DirtyBind(',
                  'void TexturePoolState::Flush(',
                  'void TexturePoolState::PurgeCaches(',
                  'bool TexturePoolState::Refresh(', 'TextureImageControl *TexturePoolState::GetHeader(',
                  'void Textures::MarkAllDirty(', 'TextureView *Textures::GetTexture(',
                  'Shader::TextureType Textures::GetTextureType(', 'bool Textures::IsTexturePixelFormatInteger(',
                  'vk::raii::BufferView *Textures::GetTextureBufferView(', 'vk::raii::BufferView *Textures::GetImageBufferView(')
    constructors = '\n'.join(line for line in source.splitlines()
                             if 'TexturePoolState::TexturePoolState(' in line or 'Textures::Textures(' in line)
    (tmp / 'methods.inc').write_text(constructors + '\n' + '\n'.join(extract(source, signature) for signature in signatures if signature in source))
    guest = (CPP / 'gpu/texture/texture.h').read_text()
    texture = (CPP / 'gpu/texture/texture.cpp').read_text()
    (tmp / 'set_mappings.inc').write_text(extract(guest, 'template<typename MemoryManager>'))
    (tmp / 'valid_mappings.inc').write_text(extract(texture, 'bool GuestTexture::MappingsValid('))
    binary = tmp / 'tests'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-parameter', '-Wno-reorder', '-Wno-type-limits',
                    '-Wno-implicit-fallthrough', '-D_GLIBCXX_ASSERTIONS', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-pthread', '-I' + str(tmp),
                    '-I' + os.environ.get('BOOST_INCLUDE_DIR', '/usr/include'),
                    str(ROOT / 'tests/gpu_texture_mappings/tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *os.sys.argv[1:]], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
