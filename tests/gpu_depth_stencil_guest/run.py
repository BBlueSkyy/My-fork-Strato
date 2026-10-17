#!/usr/bin/env python3
"""ASan/UBSan production guest synchronization tests; --ref is a negative control."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ref")
parser.add_argument("--upload-only", action="store_true")
parser.add_argument("--readback-only", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="strato-ds-guest-") as temporary:
    temporary = Path(temporary)
    source = subprocess.check_output(["git", "show", args.ref + ":app/src/main/cpp/skyline/gpu/texture/texture.cpp"], cwd=ROOT, text=True) if args.ref else (GPU / "texture/texture.cpp").read_text()
    (temporary / "production.inc").write_text("\n".join(function(source, signature) for signature in (
        "std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl(", "void Texture::CopyToGuest(")))
    layout = (GPU / "texture/layout.cpp").read_text()
    (temporary / "pitch.inc").write_text("\n".join(function(layout, signature) for signature in (
        "void CopyPitchLinearToLinear(", "void CopyLinearToPitchLinear(")))
    executable = temporary / "tests"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie",
        "-I" + str(temporary), "-I" + str(GPU.parent),
        "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
        "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
        str(Path(__file__).with_name("tests.cpp")), "-o", str(executable)], check=True)
    env = os.environ.copy()
    # LeakSanitizer cannot enumerate threads in the container's /proc view.
    # AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(executable)] + (["upload"] if args.upload_only else ["readback"] if args.readback_only else []), env=env, check=True)
