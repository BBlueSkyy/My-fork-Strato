#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
with tempfile.TemporaryDirectory(prefix='strato-feedback-') as build:
    source = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp').read_text()
    (pathlib.Path(build) / 'executor.inc').write_text(function(source, 'void CommandExecutor::AddSubpass('))
    texture = (ROOT / 'app/src/main/cpp/skyline/gpu/texture/texture.cpp').read_text()
    (pathlib.Path(build) / 'usage.inc').write_text(function(texture, 'vk::ImageUsageFlags TextureView::GetUsage(') + '\n' +
                                                function(texture, 'bool Texture::OwnsView('))
    gpu = (ROOT / 'app/src/main/cpp/skyline/gpu.cpp').read_text()
    begin = gpu.index('feedback::LayoutFeatures feedbackLayout;')
    end = gpu.index('traits.ApplyDriverPatches', begin)
    (pathlib.Path(build) / 'features.inc').write_text(gpu[begin:end])
    maxwell = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    (pathlib.Path(build) / 'dynamic.inc').write_text(function(maxwell, 'if (ctx.gpu.traits.supportsAttachmentFeedbackLoop &&'))
    updater = (ROOT / 'app/src/main/cpp/skyline/gpu/interconnect/common/state_updater.h').read_text()
    (pathlib.Path(build) / 'dynamic_command.inc').write_text(function(updater, 'struct SetAttachmentFeedbackCmdImpl') + ';\n')
    compiler = ['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I' + str(ROOT / 'app/src/main/cpp/skyline'),
                    '-I' + build,
                    '-I' + str(ROOT / 'app/libraries/vkhpp'),
                    '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include')]
    for name in ('tests', 'usage'):
        binary = pathlib.Path(build) / name
        subprocess.run(compiler + [str(ROOT / 'tests/gpu_attachment_feedback' / (name + '.cpp')), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    subprocess.run(compiler + ['-fsyntax-only', str(ROOT / 'tests/gpu_attachment_feedback/native_compile.cpp')], check=True)
    print('PASS: native feedback feature chain and RAII command dispatch compile against pinned Vulkan-Hpp')
