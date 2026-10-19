#!/usr/bin/env python3
import os,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
def extract(text,signature):
    start=text.index(signature); end=text.index('{',start)+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]
with tempfile.TemporaryDirectory(prefix='strato-sparse-mirror-') as tmp:
    tmp=pathlib.Path(tmp)
    source=(ROOT/'app/src/main/cpp/skyline/kernel/memory.cpp').read_text()
    baseline=os.environ.get('MIRROR_BASELINE_REF')
    if baseline: source=subprocess.check_output(['git','show',baseline+':app/src/main/cpp/skyline/kernel/memory.cpp'],cwd=ROOT,text=True)
    signature='span<u8> MemoryManager::CreateMirrorsWithSparse('
    (tmp/'mirror.inc').write_text(extract(source,signature) if signature in source else '')
    binary=tmp/'tests'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-I'+str(tmp),str(ROOT/'tests/gpu_sparse_mappings/tests.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
