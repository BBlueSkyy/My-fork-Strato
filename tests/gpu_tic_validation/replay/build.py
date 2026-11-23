#!/usr/bin/env python3
"""Build the repository-pinned Maxwell -> SPIR-V compiler without Android."""
import argparse, concurrent.futures, hashlib, json, os, pathlib, subprocess
p=argparse.ArgumentParser(); p.add_argument('--deps',type=pathlib.Path,required=True); p.add_argument('--boost',type=pathlib.Path,required=True); p.add_argument('--build',type=pathlib.Path,required=True); p.add_argument('--jobs',type=int,default=2); a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3]; sc=root/'app/libraries/shader-compiler'; out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['-std=c++20','-O0','-g0','-w','-include','cstddef','-DFMT_HEADER_ONLY','-fno-strict-aliasing','-ffunction-sections','-fdata-sections']
if 'clang' in os.environ.get('CXX',''): flags+=['-fbracket-depth=1024']
flags += ['-I'+str(q.resolve()) for q in [sc/'include',a.deps/'fmt/include',a.deps/'range/include',a.deps/'sirit/include',a.deps/'sirit/src',a.deps/'sirit/externals/SPIRV-Headers/include',a.boost]]
sources=sorted([*sc.glob('backend/spirv/*.cpp'),*sc.glob('frontend/**/*.cpp'),*sc.glob('ir_opt/*.cpp'),*(a.deps/'sirit/src').rglob('*.cpp')])+[pathlib.Path(__file__).with_name('replay.cpp')]
objects=[]; records=[]
def build(src):
    key=hashlib.sha256(str(src.resolve()).encode()).hexdigest()[:12]; obj=out/(src.stem+'-'+key+'.o'); stamp=obj.with_suffix('.json')
    # Hash all compiler/dependency headers as well as each TU: cached objects cannot
    # silently survive a compiler/submodule update or host-header replacement.
    cmd=[os.environ.get('CXX','c++'),*flags,'-c',str(src),'-o',str(obj)]
    record={'source':str(src.resolve()),'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'headers_sha256':headers_hash,'command':cmd}
    if not obj.exists() or not stamp.exists() or json.loads(stamp.read_text()) != record:
        r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        if r.returncode: raise RuntimeError(str(src)+'\n'+r.stdout)
        stamp.write_text(json.dumps(record,indent=2)+'\n')
    return obj,record
h=hashlib.sha256()
for q in sorted([*sc.rglob('*.h'),*(a.deps/'fmt/include').rglob('*.h'),*(a.deps/'range/include').rglob('*.hpp'),*(a.deps/'sirit/include').rglob('*.h'),*(a.deps/'sirit/externals/SPIRV-Headers/include').rglob('*.hpp')]):h.update(str(q).encode());h.update(q.read_bytes())
headers_hash=h.hexdigest()
with concurrent.futures.ThreadPoolExecutor(max_workers=min(a.jobs,3)) as pool:
    for i,(obj,rec) in enumerate(pool.map(build,sources),1):
        objects.append(obj);records.append(rec)
        if i%20==0: print(f'compiled {i}/{len(sources)}',flush=True)
binary=out/'replay';subprocess.run([os.environ.get('CXX','c++'),*[str(o) for o in objects],'-Wl,--gc-sections','-o',str(binary)],check=True)
def git_info(path):
    return {'commit':subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip(),'status':subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True)}
(out/'build-manifest.json').write_text(json.dumps({'compiler':git_info(sc),'dependencies':{name:git_info(a.deps/name) for name in ['fmt','range','sirit']},'cxx_version':subprocess.check_output([os.environ.get('CXX','c++'),'--version'],text=True),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'headers_sha256':headers_hash,'translation_units':records},indent=2)+'\n')
print(binary)
