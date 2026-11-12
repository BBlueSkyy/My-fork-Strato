#!/usr/bin/env python3
"""Compile each actual selector and verify interventions/probe reproducibility."""
import importlib.util
import os
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("pr341_prepare", HERE / "prepare.py")
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def selector(source):
    start = source.index("                const bool sameTileConfig")
    # Includes the complete parent-selection condition and mip/layer walk.
    tail = source.index("                if (matchGuestTexture.format->IsCompatible", start)
    return source[start:tail] + block(source[tail:], "if (matchGuestTexture.format->IsCompatible")


original = prep.load_original(ROOT)
with tempfile.TemporaryDirectory(prefix="strato-pr341-ab-") as temp:
    temp = pathlib.Path(temp)
    # All probe output must be identical between arms, apart from variant label.
    control = prep.generate(original, "A_control")
    # Probes must not access GPU::state: that field is private to Texture/Buffer.
    # This would be missed by the small host facade but fails the real Android compile.
    draw_probe = control[prep.DRAW]
    assert "ctx.gpu.state" not in draw_probe, "probe illegally accesses private GPU state"
    assert "guest_constants={},{},{},{}" in draw_probe, "blend constants probe lost"
    assert "pr341BlendConsts[3]);" in draw_probe, "blend constant argument list drift"
    for number, variant in enumerate(prep.VARIANTS):
        generated = prep.generate(original, variant)
        manager_without_policy = generated[prep.MANAGER]
        if number:
            manager_without_policy = prep.replace_once(manager_without_policy, prep.rejection_probe(variant), "")
            condition = "sameTileConfig" if number == 1 else "level == 0"
            manager_without_policy = prep.replace_once(manager_without_policy,
                prep.ADMISSION.replace("if (", "if (" + condition + " && ", 1), prep.ADMISSION)
        assert manager_without_policy == control[prep.MANAGER], "more than the declared selector axis changed"
        for path in original:
            if path == prep.MANAGER:
                continue
            assert generated[path] == control[path], "common probe drift"
        assert generated[prep.HEADER].replace(variant, "A_control") == control[prep.HEADER]
        # The compile test excludes only the logging statements; no selector arithmetic is replaced.
        selected = selector(prep.policy(original[prep.MANAGER], variant))
        (temp / "selector.inc").write_text(selected)
        binary = temp / variant
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                        "-DPOLICY=" + str(number), "-I" + str(temp), str(HERE / "test_selector.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        # Persist generated preview files only in the temporary test directory.
        for path, content in generated.items():
            target = temp / (variant + "-preview") / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
    # The generated header's counters are shared across TUs, bounded and thread safe.
    (temp / "trace.h").write_text(control[prep.HEADER])
    (temp / "budget.cpp").write_text('''#include "trace.h"
#include <thread>
#include <vector>
#include <cassert>
int main(){ std::atomic<unsigned> accepted{}; std::vector<std::thread> threads;
for(int i=0;i<4;i++) threads.emplace_back([&]{for(int j=0;j<1000;j++) if(skyline::gpu::diagnostic::Pr341TraceBudget()) ++accepted;});
for(auto &t:threads)t.join(); assert(accepted==512);
unsigned draws{}; for(int i=0;i<30000;i++) if(skyline::gpu::diagnostic::Pr341DrawSample())++draws;
assert(draws==128); }
''')
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-pthread", "-I" + str(temp),
                    str(temp / "budget.cpp"), "-o", str(temp / "budget")], check=True)
    subprocess.run([str(temp / "budget")], check=True)
    print("PASS identical shared probes, source guards and concurrent bounded counters")
    hpp = pathlib.Path(os.environ.get("VULKAN_HPP_INCLUDE_DIR", ROOT / "app/libraries/vkhpp"))
    headers = pathlib.Path(os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", hpp / "Vulkan-Headers/include"))
    fmt = pathlib.Path(os.environ.get("FMT_INCLUDE_DIR", ROOT / "app/libraries/fmt/include"))
    if not (hpp / "vulkan/vulkan.hpp").exists() or not (fmt / "fmt/format.h").exists():
        if os.environ.get("CI") == "true":
            raise RuntimeError("Pinned probe compile dependencies missing in CI")
        print("SKIP probe compilation: initialize Vulkan-Hpp/headers and fmt, or set their include paths")
    else:
        (temp / "manager-probe.inc").write_text(block(control[prep.MANAGER], "auto pr341Trace") + ";")
        for name, signature, end in (
            ("promotion", "void Texture::PromoteBackingForView(", "        if (grow && !CanGrowMipLevels"),
            ("reconcile", "void Texture::ReconcileAliases(", "        if (download) {"),
        ):
            method = block(control[prep.TEXTURE], signature)
            start = method.index("        if (diagnostic::Pr341BloomShape")
            (temp / (name + "-probe.inc")).write_text(method[start:method.index(end, start)])
        texture = control[prep.TEXTURE]
        start = texture.index("        for (const auto &member : textures)\n            if (diagnostic::Pr341BloomShape")
        end = texture.index("        if (!requested)", start)
        (temp / "alias-probe.inc").write_text(texture[start:end])
        draw = block(control[prep.DRAW], "void Maxwell3D::Draw(")
        start = draw.index("        if (const auto *pipeline")
        (temp / "draw-probe.inc").write_text(draw[start:draw.index("        constantBuffers.ResetQuickBind();", start)])
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-Wno-unused-but-set-variable", "-I" + str(temp),
                        "-isystem", str(hpp), "-isystem", str(headers), "-isystem", str(fmt), str(HERE / "test_probes.cpp"),
                        "-o", str(temp / "probes")], check=True)
        subprocess.run([str(temp / "probes")], check=True)
        print("PASS actual generated probe bodies with pinned Vulkan/fmt, null views and alias inputs")
