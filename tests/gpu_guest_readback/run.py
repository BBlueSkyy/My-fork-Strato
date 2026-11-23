#!/usr/bin/env python3
"""Exercise production guest synchronization and trap callbacks with gated GPU I/O."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def body(source, start):
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = (ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="strato-guest-readback-") as directory:
    directory = Path(directory)
    (directory / "sync.inc").write_text(body(source, source.index("void Texture::SynchronizeGuest(")))
    start = source.index("[weakThis]", source.index("trapHandle = gpu.state.process->trap.CreateTrap"))
    (directory / "lock.inc").write_text(body(source, start))
    for name in ("Read", "Write"):
        marker = source.index('TRACE_EVENT("gpu", "Texture::' + name + 'Trap")')
        start = source.rindex("[weakThis]", 0, marker)
        (directory / (name.lower() + ".inc")).write_text(body(source, start))
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-pthread", "-I" + str(directory), "-I" + str(ROOT / "app/src/main/cpp/skyline"),
               str(HERE / "tests.cpp"), "-o", str(directory / "tests")]
    sanitizer = os.environ.get("SANITIZE", "undefined")
    if sanitizer:
        command += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=20)
