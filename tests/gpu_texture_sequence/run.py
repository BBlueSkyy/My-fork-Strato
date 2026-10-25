#!/usr/bin/env python3
"""Compile unchanged production texture upload/copy bodies with gated GPU I/O."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PATH = "app/src/main/cpp/skyline/gpu/texture/texture.cpp"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


baseline = os.environ.get("TEXTURE_SEQUENCE_BASELINE_REF")
source = (subprocess.check_output(["git", "show", baseline + ":" + PATH], cwd=ROOT, text=True)
          if baseline else (ROOT / PATH).read_text())
with tempfile.TemporaryDirectory(prefix="strato-texture-sequence-") as directory:
    directory = Path(directory)
    (directory / "production.inc").write_text("\n".join(function(source, signature) for signature in (
        "void Texture::SynchronizeHost(", "void Texture::SynchronizeHostInline(",
        "void Texture::WaitOnFence(")))
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-pthread", "-I" + str(directory), str(HERE / "tests.cpp"), "-o", str(directory / "tests")]
    sanitizer = os.environ.get("SANITIZE", "undefined")
    if sanitizer:
        command += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "tests"), *os.sys.argv[1:]], check=True, timeout=30)
