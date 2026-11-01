#!/usr/bin/env python3
"""Bounded probe core/reporting under ASan/UBSan (no Android or Vulkan needed)."""
import os
from pathlib import Path
import subprocess
import tempfile
import argparse
root=Path(__file__).resolve().parents[2]
here=Path(__file__).resolve().parent
parser=argparse.ArgumentParser()
parser.add_argument('--deps',type=Path,default=root/'app/libraries')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='strato-readback-probe-') as temp:
    source=(root/'app/src/main/cpp/skyline/gpu/texture/texture.cpp').read_text()
    start=source.index('void Texture::WaitOnFence()');end=source.index('    void Texture::SwapBacking',start)
    (Path(temp)/'texture_wait.inc').write_text(source[start:end])
    for name in ['core','session','texture_wait']:
        output=Path(temp)/name
        command=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-O1','-g','-DSTRATO_READBACK_DIAGNOSTICS=1','-DFMT_HEADER_ONLY','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'app/src/main/cpp/skyline'),'-I'+temp,'-I'+str(args.deps/'fmt/include'),str(here/(name+'.cpp'))]
        if name=='session':command.append(str(root/'app/src/main/cpp/skyline/common/readback_probe.cpp'))
        subprocess.run(command+['-o',str(output)],check=True)
        for mode in (['normal','lifetime','late'] if name=='session' else ['']):
            subprocess.run([str(output)]+([mode] if mode else []),check=True,timeout=30)
