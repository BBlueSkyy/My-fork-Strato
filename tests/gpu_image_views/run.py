#!/usr/bin/env python3
"""Exercise exact production image-view selection/cache at the Vulkan creation boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
source = (GPU / 'texture/texture.cpp').read_text()
baseline = os.environ.get('TEXTURE_IDENTITY_BASELINE_REF')
if baseline:
    source = subprocess.check_output(['git', 'show', baseline + ':app/src/main/cpp/skyline/gpu/texture/texture.cpp'], cwd=ROOT, text=True)
method = function(source, 'vk::ImageView TextureView::GetView(')
# The negative control keeps the complete pre-fix production body unchanged.
# Its old no-argument signature is adapted solely to compile against the new API.
method = method.replace('TextureView::GetView()', 'TextureView::GetView(bool identityMapping)')
with tempfile.TemporaryDirectory(prefix='strato-image-views-') as directory:
    directory = Path(directory)
    (directory / 'production.inc').write_text(method)
    subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-fsanitize=undefined', '-fno-sanitize-recover=all', '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS',
        '-I' + str(directory), '-I' + str(ROOT / 'app/libraries/vkhpp'),
        '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
        str(Path(__file__).with_name('tests.cpp')), '-o', str(directory / 'tests')], check=True)
    subprocess.run([str(directory / 'tests')], check=True)
