#!/usr/bin/env python3
"""Adapt the existing real-Vulkan host boundary; extract current production methods."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
base = ROOT / 'tests/gpu_backing_promotion'
runner = (base / 'run.py').read_text()
source = (base / 'tests.cpp').read_text()
# Keep the established host boundary and production method inclusion. Only run
# the focused DS scenario below; existing promotion tests are never modified.
header = (ROOT / 'app/src/main/cpp/skyline/gpu/texture/texture.h').read_text()
baseline = os.environ.get('DEPTH_STENCIL_BASELINE_COMPAT_REF')
if baseline:
    header = subprocess.check_output(['git', 'show', baseline + ':app/src/main/cpp/skyline/gpu/texture/texture.h'], cwd=ROOT, text=True)
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
method = function(header, 'constexpr bool IsCompatible(').replace('FormatBase', 'NativeFormat')
source = source.replace('#include <bit>', '#include <bit>\n#include <vulkan/vulkan_format_traits.hpp>\n#include <gpu/texture/depth_stencil.h>\n#include <gpu/texture/pitch_linear.h>\n#include <ranges>\nnamespace ranges = std::ranges;\nusing u8 = uint8_t;')
source = source.replace('size_t blockWidth{1}, blockHeight{1}, bpb{4};', 'size_t blockWidth{1}, blockHeight{1}, bpb{4}; bool stencilFirst{};')
source = source.replace('bool IsCompatible(const NativeFormat &) const { return true; } // Format classes are tested by real Vulkan.', method)
# The host boundary creates an actual attachment image with the required usage.
source = source.replace('if (this->native.IsCompressed()) usage', '''if (native == vk::Format::eD24UnormS8Uint) {
                this->native.vkAspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
                usage &= ~(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eStorage);
                usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
            }
            if (this->native.IsCompressed()) usage''')
source = source.replace('struct DirtyState { static constexpr int Clean{0}, CpuDirty{1}, GpuDirty{2}; };', 'struct DirtyState { static constexpr int Clean{0}, CpuDirty{1}, GpuDirty{2}; int value{}; DirtyState(int value=0):value(value){} operator int() const {return value;} };')
source = source.replace('u32 GetLayerStride() {', 'size_t GetSize() const { return layerStride * layerCount; }\n        u32 GetLayerStride() {')
# Extract only facade definitions and helpers, followed by the DS scenario.
head, main = source.split('int main() {', 1)
head = head[:head.index('    void CheckImagelessAttachment(')] + '\n'.join(function(head, signature) for signature in ('std::vector<u8> AliasHostBytes(', 'void AliasRead(', 'void EncodeAliasBase(')) + '\n}\n'
main = 'int main() {' + main
start = main.index('            CheckSeparateLayouts(gpu);')
end = main.index('\n        }\n        CheckLogicalExtent();', start)
main = main[:start] + '            CheckDepthStencilAliases(gpu);' + main[end:]
main = main.replace('        CheckLogicalExtent();', '')
start = main.index('    std::puts(')
main = main[:start] + '    std::puts("D24 packed-order aliases: real production manager/coherence/transfers, alternating writers and cached reads PASS");\n}\n'
scenario = (ROOT / 'tests/gpu_depth_stencil_alias/scenario.inc').read_text()
source = head + '\nnamespace skyline::gpu {\n' + scenario + '\n}\n' + main
with tempfile.TemporaryDirectory(prefix='strato-d24-alias-source-') as directory:
    generated = pathlib.Path(directory) / 'tests.cpp'
    generated.write_text(source)
    runner = runner.replace('ROOT = pathlib.Path(__file__).resolve().parents[2]', 'ROOT = pathlib.Path(' + repr(str(ROOT)) + ')')
    runner = runner.replace('str(ROOT / "tests/gpu_backing_promotion/tests.cpp")', repr(str(generated)))
    exec(compile(runner, str(base / 'run.py'), 'exec'), {'__file__': str(base / 'run.py')})
