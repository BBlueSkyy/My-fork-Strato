#!/usr/bin/env python3
"""Exercise production guest/host discard APIs against shared Linux mappings."""
import pathlib, subprocess, tempfile, os
root=pathlib.Path(__file__).resolve().parents[2]
source=(root/'app/src/main/cpp/skyline/kernel/memory.cpp').read_text()
a=source.index('    __attribute__((always_inline)) void MemoryManager::FreeMemory(')
b=source.index('\n    void MemoryManager::SvcMapMemory',a)
with tempfile.TemporaryDirectory(prefix='guest-discard-memory-') as d:
 p=pathlib.Path(d)
 (p/'discard.inc').write_text(source[a:b])
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-Wno-attributes','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+d,str(root/'tests/guest_discard_memory/test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
