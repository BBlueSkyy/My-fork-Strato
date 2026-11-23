#!/usr/bin/env python3
"""Run diagnostic-only destructor probes using unchanged production bodies.

This deliberately expects the audited master's blockers. It is not a GC
acceptance suite, a full TrapManager test, or a Vulkan validation test.
"""
import argparse
import hashlib
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.cpp"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-master-blockers", action="store_true", required=True,
                        help="explicitly acknowledge that success reproduces unsafe prerequisites")
    parser.parse_args()
    source = SOURCE.read_text()
    print("Production texture.cpp sha256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="strato-texture-lifetime-audit-") as temporary:
        directory = pathlib.Path(temporary)
        bodies = "\n".join(function(source, signature) for signature in (
            "void Texture::SynchronizeGuest(", "Texture::~Texture("))
        (directory / "production.inc").write_text(bodies + "\n")
        binary = directory / "audit"
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                   "-O1", "-g", "-fno-omit-frame-pointer",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                   "-I" + str(directory),
                   str(ROOT / "tests/gpu_texture_lifetime_audit/probes.cpp"),
                   "-o", str(binary)]
        command += shlex.split(os.environ.get("CXXFLAGS", ""))
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    print("Diagnostic probes completed; eviction safety is NOT established.", flush=True)


if __name__ == "__main__":
    main()
