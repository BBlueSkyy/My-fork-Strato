#!/usr/bin/env python3
"""Compile the actual predicate methods and GLSL comparison expression on a host GPU interface."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'



def without_optional_ue4_capture(body):
    """Exclude only mini-RenderDoc probes from host-mock GPU unit builds.

    The host fixtures do not provide Vulkan textures, shader pipeline or
    diagnostic state. Keep the production draw/condition logic unchanged.
    """
    marker = "if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {"
    while marker in body:
        start = body.index(marker)
        begin = body.index('{', start)
        depth, end = 1, begin + 1
        while depth:
            depth += (body[end] == '{') - (body[end] == '}')
            end += 1
        body = body[:start] + body[end:]
    return body

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix='strato-gpu-conditional-') as directory:
    directory = pathlib.Path(directory)
    header = (GPU / 'interconnect/maxwell_3d/maxwell_3d.h').read_text()
    source = (GPU / 'interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
    declarations = header[header.index('        struct RenderConditionKey'):header.index('        InterconnectContext ctx;')]
    declarations += header[header.index('        CachedMappedBufferView renderConditionLhsView'):header.index('        static constexpr size_t DescriptorBatchSize')]
    declarations = declarations.replace('        void RefreshRenderCondition();', '')
    (directory / 'members.inc').write_text(declarations)
    methods = [source[source.index("    Maxwell3D::RenderConditionResources::RenderConditionResources("):source.index("    Maxwell3D::Maxwell3D(")]]
    for name in ('ClearRenderCondition', 'SetRenderCondition', 'CheckRenderCondition', 'RefreshRenderCondition', 'FlushQueries', 'InvalidateRenderConditionAfterDraw', 'Draw'):
        for prefix in ('void', 'bool'):
            signature = f'{prefix} Maxwell3D::{name}('
            if signature in source:
                methods.append(without_optional_ue4_capture(function(source, signature)))
    pipeline = (GPU / 'interconnect/maxwell_3d/pipeline_manager.cpp').read_text()
    active = (GPU / 'interconnect/maxwell_3d/active_state.cpp').read_text()
    (directory / 'writers.inc').write_text('\n'.join([
        function(pipeline, 'void Pipeline::InheritDescriptorBufferViews('),
        function(pipeline, 'bool Pipeline::MayWriteBuffer('),
        function(active, 'bool TransformFeedbackBufferState::MayWriteBuffer('),
        function(active, 'bool ActiveState::MayWriteTransformFeedbackBuffer(')]))
    buffer_source = (GPU / 'buffer.cpp').read_text()
    generation = '\n'.join(function(buffer_source, signature) for signature in ('void Buffer::MarkGpuDirty(', 'bool Buffer::Write(', 'void Buffer::CopyFrom('))
    (directory / 'generation.inc').write_text(generation.replace('Buffer::', 'GenerationBuffer::').replace('Buffer *src', 'GenerationBuffer *src'))
    (directory / 'production.inc').write_text('\n'.join(methods))
    engine_path = 'app/src/main/cpp/skyline/soc/gm20b/engines/maxwell_3d.cpp'
    engine = (ROOT / engine_path).read_text()
    engine = function(engine, 'bool Maxwell3D::CheckRenderEnable(').replace('bool Maxwell3D::', 'bool EngineMaxwell3D::')
    (directory / 'engine.inc').write_text(engine)

    shader = (ROOT / 'app/src/main/shaders/conditional_compare.comp').read_text()
    fields = shader[shader.index('    uint lhs'):shader.index('} scratchData;')].replace('uint ', 'u32 ')
    push = shader[shader.index('    uint notEqual'):shader.index('} pc;')].replace('uint ', 'u32 ')
    body = function(shader, 'void main()').replace('void main()', 'void ShaderMain(u32 *memory, bool notEqual, bool nonZero = false)')
    brace = body.index('{') + 1
    body = body[:brace] + '\n    struct Scratch {\n' + fields + '} &scratchData = *reinterpret_cast<Scratch *>(memory);\n    struct Push {\n' + push + '} pc{}; pc.notEqual = notEqual;\n' + ('pc.nonZero = nonZero;\n' if 'nonZero;' in push else '') + body[brace:]
    (directory / 'shader.inc').write_text(body)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I' + str(directory), str(ROOT / 'tests/gpu_conditional/tests.cpp'),
                    '-o', str(directory / 'tests')], check=True)
    subprocess.run([str(directory / 'tests')], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
