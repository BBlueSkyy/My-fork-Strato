#!/usr/bin/env python3
"""Compile the unchanged production CopyToGuest body under limited host/GPU facades."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
path = root / "app/src/main/cpp/skyline/gpu/texture/texture.cpp"
source = path.read_text()
start = source.index("void Texture::CopyToGuest(u8 *hostBuffer)")
brace = source.index("{", start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
with tempfile.TemporaryDirectory(prefix="strato-linear-readback-") as temp:
    temp = Path(temp)
    (temp / "production.inc").write_text(source[start:end] + "\n")
    executable = temp / "readback-test"
    args = [
        os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-I", str(temp), str(Path(__file__).with_name("tests.cpp")), "-o", str(executable),
    ]
    sanitizer = os.environ.get("SANITIZE", "address,undefined")
    if sanitizer:
        args += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g"]
    subprocess.run(args, check=True)
    subprocess.run([str(executable)], check=True)
