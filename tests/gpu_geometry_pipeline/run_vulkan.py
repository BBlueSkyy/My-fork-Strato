#!/usr/bin/env python3
"""Compare production emitted GS coverage/varyings against a direct VS/FS pipeline.

Pass the modules directory emitted by shader-compiler's geometry_passthrough_tests.
Requires a Vulkan graphics ICD, validation layer, glslangValidator, and spirv-val.
Environment include/library paths can select portable Vulkan dependencies.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modules", type=Path)
    parser.add_argument("--require-synthetic", action="store_true", help="fail if synthetic Layer GS fixtures are absent")
    args = parser.parse_args()
    for tool in ("g++", "glslangValidator", "spirv-val"):
        if not shutil.which(tool):
            raise RuntimeError("required tool unavailable: " + tool)
    modules = args.modules.resolve()
    for name in ("points", "lines", "triangles"):
        path = modules / (name + "-lowered.spv")
        if not path.is_file():
            raise RuntimeError("missing production emitted module: " + str(path))
    with tempfile.TemporaryDirectory(prefix="strato-geometry-vulkan-") as directory:
        tmp = Path(directory)
        executable = tmp / "geometry-vulkan"
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", str(HERE / "vulkan.cpp"),
                        "-l:libvulkan.so.1", "-o", str(executable)], check=True)
        vertex = """#version 450
layout(location=0) out vec4 color;
layout(location=1) out vec4 emulatedLayer;
layout(location=2) out vec4 emulatedViewport;
vec2 positions[3] = vec2[](vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(0.0,0.75));
void main() { gl_Position=vec4(positions[gl_VertexIndex%3],0.5,1.0);
              gl_PointSize=1.0; emulatedLayer=vec4(0); emulatedViewport=vec4(0); color=vec4((positions[gl_VertexIndex%3]+1.0)*0.5,0.75,1.0); }
"""
        fragment = """#version 450
layout(location=0) in vec4 color;
void main() {
    // Point/line vertices can fall between sample centers by half a pixel.
    vec4 expected=vec4(gl_FragCoord.xy/32.0,0.75,1.0);
    if (any(greaterThan(abs(color-expected),vec4(0.02,0.02,0.001,0.001)))) discard;
}
"""
        for stage, source in (("vert", vertex), ("frag", fragment)):
            path = tmp / ("test." + stage)
            path.write_text(source)
            subprocess.run(["glslangValidator", "-V", str(path), "-o", str(path) + ".spv"], check=True)
        for name, values, count in (
            ("lines", "vec2(0.8,0.8),vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(-0.8,0.8)", 4),
            ("triangles", "vec2(-0.75,-0.75),vec2(0.8,0.8),vec2(0.75,-0.75),vec2(-0.8,0.8),vec2(0.0,0.75),vec2(0,-0.8)", 6)):
            source = re.sub(r"vec2 positions\[3\] = .*?;", f"vec2 positions[{count}] = vec2[]({values});", vertex)
            source = source.replace("gl_VertexIndex%3", f"gl_VertexIndex%{count}")
            path = tmp / (name + "-adj.vert")
            path.write_text(source)
            subprocess.run(["glslangValidator", "-V", str(path), "-o", str(path) + ".spv"], check=True)
        env = os.environ.copy()
        env.setdefault("XDG_RUNTIME_DIR", str(tmp))
        for topology, vertices in (("points", 1), ("lines", 2), ("triangles", 3)):
            baseline = subprocess.run([str(executable), str(tmp / "test.vert.spv"), str(tmp / "test.frag.spv"),
                                       "-", topology, str(vertices)], env=env, text=True, capture_output=True)
            print(baseline.stdout, end="")
            if baseline.returncode:
                print(baseline.stderr, end="")
                return baseline.returncode
            expected = int(re.search(r"SAMPLES=(\d+)", baseline.stdout)[1])
            if expected == 0:
                raise RuntimeError(f"{topology} baseline produces no samples")
            cases = [("lowered", topology + "-lowered.spv", topology, vertices, "test.vert.spv"),
                     ("synthetic", topology + "-synthetic.spv", topology, vertices, "test.vert.spv")]
            if topology != "points":
                cases.append(("synthetic-adjacency", topology + "-adjacency.spv", topology + "-adj",
                              4 if topology == "lines" else 6, topology + "-adj.vert.spv"))
            for kind, filename, input_topology, input_vertices, vertex_file in cases:
                module = modules / filename
                if kind.startswith("synthetic") and not module.is_file():
                    if args.require_synthetic:
                        raise RuntimeError("missing synthetic module: " + str(module))
                    print("SKIP synthetic fixture not emitted: " + module.name)
                    continue
                subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(module)], check=True)
                run = subprocess.run([str(executable), str(tmp / vertex_file), str(tmp / "test.frag.spv"),
                                      str(module), input_topology, str(input_vertices)], env=env, text=True, capture_output=True)
                print(run.stdout, end="")
                if run.returncode:
                    print(run.stderr, end="")
                    return run.returncode
                actual = int(re.search(r"SAMPLES=(\d+)", run.stdout)[1])
                if expected != actual:
                    raise RuntimeError(f"{topology}/{kind} coverage/varying mismatch: direct={expected}, GS={actual}")
                print(f"PASS {topology}/{kind}: direct and GS each produce {expected} samples with correct per-vertex varying values")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
