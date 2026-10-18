#!/usr/bin/env python3
"""Compile the real FenceCycle and complete SubmitInternal against host facades."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent

def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

with tempfile.TemporaryDirectory(prefix="strato-submission-") as directory:
    directory = pathlib.Path(directory)
    def source(path):
        baseline = os.environ.get("BUFFER_BASELINE_REF") if path.endswith("/buffer.h") else None
        baseline = baseline or os.environ.get("SUBMISSION_BASELINE_REF")
        return subprocess.check_output(["git", "show", baseline + ":" + path], cwd=ROOT, text=True) if baseline else (ROOT / path).read_text()

    (directory / "gpu").mkdir()
    (directory / "gpu/fence_cycle.h").write_text(source("app/src/main/cpp/skyline/gpu/fence_cycle.h"))
    executor = ROOT / "app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp"
    (directory / "executor.inc").write_text(function(source(str(executor.relative_to(ROOT))), "void CommandExecutor::SubmitInternal()"))
    texture = ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.cpp"
    (directory / "texture.inc").write_text(function(source(str(texture.relative_to(ROOT))), "void Texture::SynchronizeHostInline("))
    buffer_header = source("app/src/main/cpp/skyline/gpu/buffer.h")
    (directory / "buffer_cycle.inc").write_text(function(buffer_header, "void UpdateCycle("))
    buffer_source = source("app/src/main/cpp/skyline/gpu/buffer.cpp")
    (directory / "buffer.inc").write_text("\n".join(function(buffer_source, signature) for signature in (
        "void Buffer::WaitOnFence(", "void Buffer::SynchronizeHost(")))
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
               "-I" + str(HERE / "stubs"), "-I" + str(directory), "-I" + str(ROOT / "app/src/main/cpp/skyline"),
               str(HERE / "tests.cpp"), "-o", str(directory / "tests")]
    sanitizer = os.environ.get("SANITIZE", "undefined")
    if sanitizer:
        command += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=30)
