#!/usr/bin/env python3
"""Exercise production index-cache methods and dirty tracking against a binding sink."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
BASELINE = os.environ.get("INDEX_BINDING_BASELINE_REF")


def source(path):
    if BASELINE:
        return subprocess.check_output(["git", "show", f"{BASELINE}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def block(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def without_includes(text):
    return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))


with tempfile.TemporaryDirectory(prefix="strato-index-binding-") as temporary:
    directory = pathlib.Path(temporary)
    prefix = "app/src/main/cpp/skyline/"
    active = source(prefix + "gpu/interconnect/maxwell_3d/active_state.cpp")
    header = source(prefix + "gpu/interconnect/maxwell_3d/active_state.h")
    dirty = source(prefix + "common/dirty_tracking.h")
    (directory / "dirty.inc").write_text(without_includes(dirty))
    (directory / "index_class.inc").write_text(block(header, "class IndexBufferState :") + ";")
    signatures = (
        "static vk::DeviceSize GetIndexBufferSize(",
        "static vk::IndexType ConvertIndexType(",
        "static BufferBinding GenerateQuadConversionIndexBuffer(",
        "void IndexBufferState::EngineRegisters::DirtyBind(",
        "void IndexBufferState::Flush(",
        "bool IndexBufferState::Refresh(",
        "void IndexBufferState::PurgeCaches(",
    )
    constructor = next(line for line in active.splitlines() if "IndexBufferState::IndexBufferState(" in line)
    assert constructor.rstrip().endswith("{}"), "update constructor extraction for a multiline definition"
    (directory / "index_methods.inc").write_text(constructor + "\n" + "\n".join(block(active, name) for name in signatures))
    invalidator = "void ActiveState::InvalidateIndexBuffer("
    if invalidator in active:
        method = block(active, invalidator)
    elif BASELINE:
        print("NEGATIVE CONTROL: production invalidator absent; using an explicitly empty host method", flush=True)
        method = "void ActiveState::InvalidateIndexBuffer() {}"
    else:
        raise RuntimeError("production InvalidateIndexBuffer is missing; select INDEX_BINDING_BASELINE_REF for the explicit negative control")
    (directory / "invalidate.inc").write_text(method)
    updater = source(prefix + "gpu/interconnect/common/state_updater.h")
    observer = "bool HasIndexBufferBinding() const"
    observer_body = "#define INDEX_OBSERVER_AVAILABLE 0\n"
    if observer in updater:
        observer_body = "#define INDEX_OBSERVER_AVAILABLE 1\n" + block(updater, observer)
    elif not BASELINE:
        raise RuntimeError("production HasIndexBufferBinding observer is missing")
    (directory / "observer.inc").write_text(observer_body)
    (directory / "quads.inc").write_text(
        without_includes(source(prefix + "gpu/interconnect/conversion/quads.h")) + "\n" +
        without_includes(source(prefix + "gpu/interconnect/conversion/quads.cpp")))

    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-Wno-type-limits", "-Wno-reorder", "-Wno-unknown-pragmas", "-I" + str(directory),
               str(HERE / "tests.cpp"), "-o", str(directory / "tests")]
    sanitizer = os.environ.get("SANITIZE", "undefined")
    if sanitizer:
        command += ["-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=15)
