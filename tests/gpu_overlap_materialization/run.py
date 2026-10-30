#!/usr/bin/env python3
"""Compile production overlap publication/retry code against gated resources."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"
HERE = Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


baseline = os.environ.get("OVERLAP_MATERIALIZATION_BASELINE_REF")
def source(path):
    return (subprocess.check_output(["git", "show", baseline + ":" + str(path.relative_to(ROOT))], cwd=ROOT, text=True)
            if baseline else path.read_text())


manager = source(GPU / "texture_manager.cpp")
with tempfile.TemporaryDirectory(prefix="strato-overlap-materialization-") as directory:
    directory = Path(directory)
    if "void TextureManager::SynchronizeGuestOverlaps(" in manager:
        code = function(manager, "void TextureManager::SynchronizeGuestOverlaps(")
    else:
        start = manager.index("        for (auto &texture : matches)\n")
        end = manager.index("        // Create a texture", start)
        code = "void TextureManager::SynchronizeGuestOverlaps(std::vector<std::shared_ptr<Texture>> matches, ContextTag) {\n" + \
               "std::shared_ptr<Group> separateGroup;\n" + manager[start:end] + "\n}\n"
    (directory / "publication.inc").write_text(code)
    executor = source(GPU / "interconnect/command_executor.cpp")
    (directory / "retry.inc").write_text(function(executor,
        "void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &"))
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(directory),
                    str(HERE / "tests.cpp"), "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=20)
