#!/usr/bin/env python3
"""Validate this instrumentation, without rerunning prior authority audits."""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("analysis", ROOT / "tools/analyze_physical_authority.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)
compiler = os.environ.get("CXX", "c++")
flags = shlex.split(os.environ.get("CXXFLAGS", ""))
with tempfile.TemporaryDirectory(prefix="strato-physical-trace-") as temp:
    temp = Path(temp)
    sampler = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/common/samplers.h").read_text()
    start = sampler.index("template<class V> void TraceLod(")
    end = sampler.index("{", start) + 1
    depth = 1
    while depth:
        depth += (sampler[end] == "{") - (sampler[end] == "}")
        end += 1
    (temp / "sampler-trace.inc").write_text(sampler[start:end])
    common = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
              "-Wno-deprecated-declarations", "-I" + str(temp), "-I" + str(ROOT / "app/src/main/cpp/skyline")]
    subprocess.run(common + ["-DSTRATO_PHYSICAL_AUTHORITY_TRACE=1", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(Path(__file__).with_name("tests.cpp")),
                            "-o", str(temp / "enabled")] + flags, check=True)
    run = subprocess.run([str(temp / "enabled")], check=True, text=True, capture_output=True, timeout=30)
    sessions = analysis.analyze(run.stdout)
    assert len(sessions) == 5
    for horizon in sessions[:2]:
        assert horizon["ended"] and horizon["events"] < 40 and horizon["suppressed_activity"] > 59000
        assert horizon["critical_seen"] >= 6 and horizon["lost"] == 0
        assert horizon["sampling"] == "mip-growth-v3"
    summary = sessions[2:3]
    assert len(summary) == 1 and summary[0]["ended"] and summary[0]["lost"] == 0
    assert summary[0]["discard_results"][0]["result"] == -1
    assert summary[0]["discard_results"][0]["protection"] == -1
    assert summary[0]["hdr_prefix_only"][0]["nan"] == 1
    assert summary[0]["hdr_prefix_only"][0]["excess"] == 1
    assert summary[0]["confirmed_menu_violation"] is None
    import json
    raw = [json.loads(s.split("[GPU-PHYS] ",1)[1]) for s in run.stdout.splitlines()]
    growth = [e for e in raw if e.get("kind") == "growth-texel" and e.get("d") == 12345]
    assert len(growth) == 13 and sum(bool(e["detail"][2]) for e in growth) == 12
    assert {e["view"][0] for e in growth} == set(range(1,8))
    assert {e["view"][1] for e in growth if e["view"][0] == 1} == set(range(6))
    consumer = [e for e in raw if e.get("kind") == "consumer-added-levels" and e.get("shaders", [0])[0] == 10]
    assert len(consumer) == 1 and consumer[0]["destinations"] and consumer[0]["detail"][0] == 1
    rejected = next(e for e in raw if e.get("kind") == "growth-route" and e.get("c") == 1)
    assert rejected["a"] == 8 and rejected["b"] == 0
    assert any(e.get("kind") == "growth-alternate" and e.get("shape",[0]*5)[4] == 8 for e in raw)
    writer = next(e for e in raw if e.get("kind") == "growth-writer")
    known = next(e for e in raw if e.get("kind") == "growth-writer" and e.get("image") == 901)
    assert known["detail"][1] == 1 and known["hash"] == 444 and known["sample"][1] == 8
    assert writer["detail"][:2] == [1,0]  # submission observed, GPU content not known
    clear = next(e for e in raw if e.get("kind") == "clear-planned")
    assert clear["clear_known"] and clear["clear"][1] == 1065353216 and clear["view"] == [0,0,1]
    assert any(e["kind"] == "shared_sample_changed" for e in summary[0]["observations"])
    try:
        analysis.analyze(run.stdout.replace('"seq":2,', '"seq":22,', 1))
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete/reordered trace accepted")
    disabled = temp / "disabled.cpp"
    disabled.write_text('#include <gpu/texture/physical_authority_trace.h>\nint main(){int n=0;GPU_PHYS_TRACE(++n);return n;}\n')
    subprocess.run(common + [str(disabled), "-o", str(temp / "disabled")] + flags, check=True)
    subprocess.run([str(temp / "disabled")], check=True)
    output = os.environ.get("PHYSICAL_TRACE_TEST_OUTPUT")
    if output:
        Path(output).write_text(run.stdout)
    print(run.stderr.strip())
    print("PASS JSON parser, incomplete-trace rejection, HDR prefix and disabled no-evaluation gate")

# New hooks may not introduce any GPU operation or fence polling/waiting.
header = (ROOT / "app/src/main/cpp/skyline/gpu/texture/physical_authority_trace.h").read_text()
for forbidden in ("Wait(", "WaitSubmit(", "Poll(", "vkCmd", "Submit(", "AllocateStaging", "sleep("):
    assert forbidden not in header, forbidden
assert 'STRATO_PHYSICAL_AUTHORITY_TRACE "Temporary bounded physical texture authority diagnostics" OFF' in (ROOT / "app/CMakeLists.txt").read_text()
print("PASS no added GPU operations/waits; CMake option defaults OFF")

subprocess.run(['python3', str(Path(__file__).with_name('check_hooks.py'))], check=True)
