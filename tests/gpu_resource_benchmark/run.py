#!/usr/bin/env python3
"""Source-body protocol microbenchmark; not an Android/device FPS benchmark."""
import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--ref',default='HEAD',help='Committed tree to measure (isolated archive).')
parser.add_argument('--output',type=Path)
parser.add_argument('--activate-mixed',action='store_true',help='Activate only the archived test header; never changes the working tree.')
parser.add_argument('--sanitize',default='none',choices=['none','undefined','address,undefined'])
parser.add_argument('--cpu',type=int,default=min(os.sched_getaffinity(0)),help='Pin only the measured control thread; compilation and existing Vulkan workers stay unpinned.')
args=parser.parse_args()
sha=subprocess.check_output(['git','rev-parse',args.ref],cwd=ROOT,text=True).strip()
with tempfile.TemporaryDirectory(prefix='strato-resource-bench-') as tmp:
    tree=Path(tmp)
    archive=subprocess.check_output(['git','archive',sha,'app/src/main/cpp','tests'],cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as package:package.extractall(tree,filter='data')
    if args.activate_mixed:
        header=tree/'app/src/main/cpp/skyline/gpu/texture/region_authority.h'
        header.write_text(header.read_text().replace('MixedRegionAuthorityEnabled{false}','MixedRegionAuthorityEnabled{true}'))
    fixture=tree/'tests/gpu_backing_promotion/tests.cpp'
    source=fixture.read_text()
    source='#include '+json.dumps(str(HERE/'allocation_probe.inc'))+'\n'+source
    marker='\n}\n\nint main() {'
    assert source.count(marker)==1
    source=source.replace(marker,'\n#include '+json.dumps(str(HERE/'benchmark.inc'))+marker)
    if args.activate_mixed:
        source=source.replace(marker,'\n#include '+json.dumps(str(HERE/'mixed_benchmark.inc'))+marker)
    # The correctness facade uses std::vector; use actual production inline
    # capacities here so its artificial allocations do not masquerade as cost.
    source=source.replace('std::vector<Texture *> regionOperationUses;', 'boost::container::small_vector<Texture *,8> regionOperationUses;')
    source=source.replace('std::vector<std::unique_lock<std::recursive_mutex>> states, metadata;', 'boost::container::small_vector<std::unique_lock<std::recursive_mutex>,8> states; boost::container::small_vector<std::unique_lock<std::recursive_mutex>,4> metadata;')
    mixed_call='CheckMixedResourceBenchmark(gpu,formatList);' if args.activate_mixed else ''
    # Insert before all correctness modes, including newly added mapping probes.
    source=source.replace('            GPU gpu{device, physical, family, formatList};',
        '            GPU gpu{device, physical, family, formatList};\n            if (std::getenv("TEXMAN_AUTHORITY_BENCH")){CheckResourceBenchmark(gpu,formatList);'+mixed_call+'continue;}',1)
    source=source.replace('if (std::getenv("TEXMAN_BUFFER_IMAGE_PROBE") ||', 'if (std::getenv("TEXMAN_AUTHORITY_BENCH") || std::getenv("TEXMAN_BUFFER_IMAGE_PROBE") ||',1)
    source=source.replace('std::vector<std::unique_lock<RecursiveSpinLock>> bufferStates;', 'boost::container::small_vector<std::unique_lock<RecursiveSpinLock>,8> bufferStates;')
    fixture.write_text(source)
    runner=tree/'tests/gpu_backing_promotion/run.py'
    code=runner.read_text().replace('"-fsanitize=" + os.environ.get("SANITIZE", "undefined"),','*([] if os.environ.get("SANITIZE")=="none" else ["-fsanitize=" + os.environ.get("SANITIZE", "undefined")]),')
    runner.write_text(code)
    env=dict(os.environ,TEXMAN_AUTHORITY_BENCH='1',SANITIZE=args.sanitize,LP_NUM_THREADS='2',TEXMAN_BENCH_CPU=str(args.cpu))
    env['CXXFLAGS']=env.get('CXXFLAGS','')+' -O2'
    # Optimized timings are separate from the mandatory sanitizer correctness gate.
    run=subprocess.run(['python3',str(tree/'tests/gpu_backing_promotion/run.py')],env=env,text=True,capture_output=True)
    print(run.stdout,end='')
    if run.returncode:print(run.stderr);raise SystemExit(run.returncode)
    rows=[json.loads(line.removeprefix('BENCH_JSON '))for line in run.stdout.splitlines()if line.startswith('BENCH_JSON ')]
    assert len(rows)==(18 if args.activate_mixed else 12)
    for row in rows:
        if 'median_ns' in row:
            assert not any(row[key] for key in ('allocations_per_1000','copy_calls_per_1000','barrier_calls_per_1000')), row
    result={'commit':sha,'mixed_test_gate':args.activate_mixed,'compiler':env.get('CXX','c++'),'sanitizer':args.sanitize,'optimization':'-O2','control_cpu':args.cpu,'lavapipe_threads':2,'device':'host Vulkan ICD from environment; no phone FPS claim','rows':rows}
    if args.output:args.output.write_text(json.dumps(result,indent=2)+'\n')
