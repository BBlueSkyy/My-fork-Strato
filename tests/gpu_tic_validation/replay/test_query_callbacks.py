#!/usr/bin/env python3
"""Compile/run the actual compiler Environment's metadata callback contract."""
import argparse, os, pathlib, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--deps',type=pathlib.Path,required=True);p.add_argument('--boost',type=pathlib.Path);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];sc=root/'app/libraries/shader-compiler'
boost=[a.boost] if a.boost else list((a.deps/'boost/libs').glob('*/include'))
with tempfile.TemporaryDirectory(prefix='strato-query-contract-') as tmp:
    binary=pathlib.Path(tmp)/'query-callbacks'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g0','-include','cstddef','-DFMT_HEADER_ONLY','-UNDEBUG','-fsanitize=undefined','-fno-sanitize-recover=all',*[f'-I{x}' for x in [sc/'include',a.deps/'fmt/include',*boost]],str(pathlib.Path(__file__).with_name('query_callbacks.cpp')),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: default read order, duplicate reads, OR/shifts, all five query kinds; override tracks source and preserves results without scalar CBUF callbacks')
