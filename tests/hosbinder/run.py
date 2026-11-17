#!/usr/bin/env python3
"""Compile unchanged production producer methods against host-only resource interfaces."""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = "app/src/main/cpp/skyline/services/hosbinder/GraphicBufferProducer.cpp"
HEADER = "app/src/main/cpp/skyline/services/hosbinder/GraphicBufferProducer.h"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--ref", help="Compile the methods from this git revision")
parser.add_argument("--benchmark", action="store_true")
args = parser.parse_args()
def read(path):
    return (subprocess.check_output(["git", "show", f"{args.ref}:{path}"], cwd=ROOT,
                                    text=True) if args.ref else (ROOT / path).read_text())


source, header = read(SOURCE), read(HEADER)
methods = (
    "u32 GraphicBufferProducer::GetPendingBufferCount(",
    "AndroidStatus GraphicBufferProducer::RequestBuffer(",
    "AndroidStatus GraphicBufferProducer::DequeueBuffer(",
    "AndroidStatus GraphicBufferProducer::QueueBuffer(",
    "void GraphicBufferProducer::CancelBuffer(",
    "AndroidStatus GraphicBufferProducer::Disconnect(",
    "AndroidStatus GraphicBufferProducer::SetPreallocatedBuffer(",
)
with tempfile.TemporaryDirectory(prefix="strato-hosbinder-") as directory:
    directory = pathlib.Path(directory)
    (directory / "production.inc").write_text("\n\n".join(function(source, s) for s in methods))
    (directory / "production_slots.inc").write_text("\n".join(
        function(header, s) + ";" for s in ("enum class BufferState", "struct BufferSlot")))
    subprocess.run(["g++", "-std=c++20", "-O2", "-g", "-pthread", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I" + str(directory), str(ROOT / "tests/hosbinder/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")] + (["--benchmark"] if args.benchmark else []),
                   check=True)
