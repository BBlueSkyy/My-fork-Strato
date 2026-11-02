#!/usr/bin/env python3
"""Compile the unchanged production AddSubpass body against host boundaries."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = "app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp"


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix="strato-executor-diagnostics-") as directory:
    directory = pathlib.Path(directory)
    # Optional historical source verifies the identical assertions against HEAD.
    source = (subprocess.check_output(["git", "show", os.environ["STRATO_TEST_REF"] + ":" + SOURCE], cwd=ROOT, text=True)
              if "STRATO_TEST_REF" in os.environ else (ROOT / SOURCE).read_text())
    (directory / "production.inc").write_text(function(source, "void CommandExecutor::AddSubpass("))
    binary = directory / "tests"
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I" + str(directory), "-I" + str(ROOT / "app/src/main/cpp"),
                    str(ROOT / "tests/gpu_executor_diagnostics/tests.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(directory)], check=True)
