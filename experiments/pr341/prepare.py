#!/usr/bin/env python3
"""Prepare an explicitly experimental APK; never used by ordinary builds.

All arms get identical bounded, read-only probes. B and C each change one
FindOrCreate admission predicate. Retain exact generated diff/hash provenance.
"""
import argparse
import difflib
import hashlib
import json
import pathlib
import subprocess

BASE = "505bc7d1a572f415032c908989a236c48b1ba533"
GPU = "app/src/main/cpp/skyline/gpu/"
MANAGER = GPU + "texture_manager.cpp"
TEXTURE = GPU + "texture/texture.cpp"
DRAW = GPU + "interconnect/maxwell_3d/maxwell_3d.cpp"
DRAW_H = GPU + "interconnect/maxwell_3d/maxwell_3d.h"
HEADER = GPU + "texture/pr341_lab_trace.h"
GRADLE = "app/build.gradle"
DEV_STRINGS = "app/src/dev/res/values/strings.xml"
EXPECTED = {
    MANAGER: "7609aae9f5652ebc1fe7c9ee81b887a72e16b951ea2f59ea9b678056652b7ed0",
    TEXTURE: "2ce38f9401034a19728b8c86dd862500d1fd440cc544cabfaeb338fcc554117d",
    DRAW: "abb16c60423421353a370a619dcb0ad03caeca3c4a3c40fc77277a6d760ab16d",
    DRAW_H: "652b565b68a032094bf10aec3a91aef868a7fabe65abd8ca93e9c944cf292b37",
    GRADLE: "883d6035d7ceaa683992bc8f9be79f8a34e4363480822fdee3695d88a8f35c41",
    DEV_STRINGS: "76f8790cb7505ce403ae2f36d122ff714ebab3b672210cb3a8235412f2d3e384",
}
VARIANTS = ("A_control", "B_strict_tile", "C_base_mip_only")
ADMISSION = """                                    if (hostMapping->texture->ViewRangeFits(guestTexture.viewType, range) &&
                                        (range.layerCount == 1 || matchGuestTexture.GetLayerStride() == requestedStride)) {"""


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Expected exactly one anchor: {old[:100]!r}")
    return source.replace(old, new, 1)


def policy(source, variant):
    if variant == "B_strict_tile":
        return replace_once(source, ADMISSION, ADMISSION.replace("if (", "if (sameTileConfig && ", 1))
    if variant == "C_base_mip_only":
        return replace_once(source, ADMISSION, ADMISSION.replace("if (", "if (level == 0 && ", 1))
    if variant != "A_control":
        raise ValueError(variant)
    return source


def rejection_probe(variant):
    if variant == "A_control":
        return ""
    condition, reason = ("!sameTileConfig", "REJECT_TILE") if variant == "B_strict_tile" else ("level != 0", "REJECT_NONZERO")
    # Only report a rejection at an exact boundary whose dimensions/layout,
    # preference, range and stride would all have passed the control arm.
    return (ADMISSION.replace("if (", "if (" + condition + " && ", 1)[:-1] +
            '\n                                        pr341Trace("' + reason + '", hostMapping->texture, level, layer);\n')


def generate(original, variant):
    result = dict(original)
    result[GRADLE] = replace_once(result[GRADLE], 'applicationIdSuffix = ".dev"', 'applicationIdSuffix = ".pr341lab"')
    result[DEV_STRINGS] = replace_once(result[DEV_STRINGS], '>Skyline Dev</string>', '>Strato PR341 Lab</string>')
    header = r'''// SPDX-License-Identifier: MPL-2.0
// GENERATED EXPERIMENT ONLY. Never included by a normal repository build.
#pragma once
#include <atomic>
#include <cstdint>
namespace skyline::gpu::diagnostic {
    inline constexpr const char *Pr341Variant = "VARIANT";
    inline bool Pr341BloomShape(uint32_t w, uint32_t h) { return w == 320 && h == 180; }
    inline bool Pr341TraceBudget() {
        static std::atomic<uint32_t> count{};
        auto seen = count.load(std::memory_order_relaxed);
        while (seen < 512)
            if (count.compare_exchange_weak(seen, seen + 1, std::memory_order_relaxed)) return true;
        return false;
    }
    inline bool Pr341DrawSample() {
        static std::atomic<uint64_t> seen{};
        static std::atomic<uint32_t> written{};
        const auto ordinal = seen.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal > 16 && ordinal % 128) return false;
        auto count = written.load(std::memory_order_relaxed);
        while (count < 128)
            if (written.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) return true;
        return false;
    }
}
'''.replace('"VARIANT"', json.dumps(variant))
    result[HEADER] = header
    for path in (MANAGER, TEXTURE, DRAW):
        result[path] = '#include <gpu/texture/pr341_lab_trace.h>\n' + result[path]

    manager = result[MANAGER]
    manager = replace_once(manager,
        "TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}",
        'TextureManager::TextureManager(GPU &gpu) : gpu(gpu) { LOGI("PR341_AB_VARIANT {}", diagnostic::Pr341Variant); }')
    anchor = "        const auto requestedStride{guestTexture.layerStride ? guestTexture.layerStride : guestTexture.CalculateLayerSize()};"
    manager = replace_once(manager, anchor, anchor + r'''
        auto pr341Trace{[&](const char *decision, const std::shared_ptr<Texture> &parent, u32 mip, u32 layer) {
            if (!parent || !diagnostic::Pr341BloomShape(parent->dimensions.width, parent->dimensions.height) ||
                parent->format->vkFormat != vk::Format::eB10G11R11UfloatPack32 || !diagnostic::Pr341TraceBudget()) return;
            LOGI("PR341_AB_LOOKUP {} request={}x{} mips={} view={}+{} layer={}+{} map=0x{:X} bytes={} gob={}/{} parent=0x{:X} parent_mips={} parent_gob={}/{} selected_mip={} selected_layer={}",
                decision, guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.mipLevelCount,
                requestedRange.baseMipLevel, requestedRange.levelCount, requestedRange.baseArrayLayer, requestedRange.layerCount,
                reinterpret_cast<uintptr_t>(guestMapping.data()), guestMapping.size(),
                guestTexture.tileConfig.mode == texture::TileMode::Block ? guestTexture.tileConfig.blockHeight : 0,
                guestTexture.tileConfig.mode == texture::TileMode::Block ? guestTexture.tileConfig.blockDepth : 0,
                reinterpret_cast<uintptr_t>(parent.get()), parent->levelCount,
                parent->guest->tileConfig.mode == texture::TileMode::Block ? parent->guest->tileConfig.blockHeight : 0,
                parent->guest->tileConfig.mode == texture::TileMode::Block ? parent->guest->tileConfig.blockDepth : 0, mip, layer);
        }};
''')
    manager = replace_once(manager, "        } else if (layerMipMatch) {\n", "        } else if (layerMipMatch) {\n            pr341Trace(\"MIP_REUSE\", layerMipMatch, matchLevel, matchLayer);\n")
    manager = replace_once(manager, "        } else if (fullMatch) {\n", "        } else if (fullMatch) {\n            pr341Trace(\"FULL_REUSE\", fullMatch, requestedRange.baseMipLevel, requestedRange.baseArrayLayer);\n")
    manager = replace_once(manager, "        texture->SetupGuestMappings();", '        texture->SetupGuestMappings();\n        pr341Trace("CREATE", texture, requestedRange.baseMipLevel, requestedRange.baseArrayLayer);')
    if variant != "A_control":
        manager = replace_once(manager, ADMISSION, rejection_probe(variant) + ADMISSION)
    result[MANAGER] = policy(manager, variant)

    texture = result[TEXTURE]
    anchor = "        if (grow && !CanGrowMipLevels(nextLevels))"
    texture = replace_once(texture, anchor, r'''        if (diagnostic::Pr341BloomShape(dimensions.width, dimensions.height) &&
            format->vkFormat == vk::Format::eB10G11R11UfloatPack32 && diagnostic::Pr341TraceBudget())
            LOGI("PR341_AB_PROMOTE phase=attempt texture=0x{:X} levels={} -> {} format_change={}",
                reinterpret_cast<uintptr_t>(this), levelCount, nextLevels, admitFormat);
''' + anchor)
    anchor = "        if (download) {\n            if (!writer)"
    texture = replace_once(texture, anchor, r'''        if (diagnostic::Pr341BloomShape(dimensions.width, dimensions.height) &&
            format->vkFormat == vk::Format::eB10G11R11UfloatPack32 && diagnostic::Pr341TraceBudget())
            LOGI("PR341_AB_RECONCILE phase=attempt target=0x{:X} writer=0x{:X} generation={} download={}",
                reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(writer.get()), generation, download);
''' + anchor)
    anchor = "        for (auto &texture : textures) locks.emplace_back(*texture);"
    texture = replace_once(texture, anchor, anchor + r'''
        for (const auto &member : textures)
            if (diagnostic::Pr341BloomShape(member->dimensions.width, member->dimensions.height) &&
                member->format->vkFormat == vk::Format::eB10G11R11UfloatPack32 && diagnostic::Pr341TraceBudget())
                LOGI("PR341_AB_ALIAS_REGISTER phase=attempt member=0x{:X} levels={} members={} requested_levels={}",
                    reinterpret_cast<uintptr_t>(member.get()), member->levelCount, textures.size(), requested ? requested->mipLevelCount : 0);
''')
    result[TEXTURE] = texture

    result[DRAW_H] = replace_once(result[DRAW_H], "        Textures textures;\n        const u32 &sampleCounterEnable;",
        "        Textures textures;\n        const u32 &sampleCounterEnable;\n        const std::array<float, engine::BlendColorChannelCount> &pr341BlendConsts;")
    draw = replace_once(result[DRAW], "          sampleCounterEnable{registerBundle.sampleCounterEnable},",
        "          sampleCounterEnable{registerBundle.sampleCounterEnable},\n          pr341BlendConsts{registerBundle.activeStateRegisters.blendConstantsRegisters.blendConsts},")
    # This exact site is Draw, not InlineDraw/IndirectDraw; captures used indexed Draw.
    anchor = '        vk::Rect2D scissor{GetDrawScissor()};\n\n\n        constantBuffers.ResetQuickBind();'
    probe = r'''
        if (const auto *pipeline{activeState.GetPipeline()}) {
            const auto fs{pipeline->sourcePackedState.shaderHashes[5]};
            if ((fs == 0x05EB6F31815A3BF8ULL || fs == 0xF8F8901DE46A8AFFULL) && diagnostic::Pr341DrawSample()) {
                const auto &blend{pipeline->sourcePackedState.attachmentBlendStates[0]};
                LOGI("PR341_AB_DRAW fs={:016X} clip={}x{} srcStages=0x{:X} dstStages=0x{:X} blend={} src={} dst={} guest_constants={},{},{},{}",
                    fs, scissor.extent.width, scissor.extent.height, static_cast<uint32_t>(srcStageMask), static_cast<uint32_t>(dstStageMask),
                    static_cast<int>(blend.blendEnable), static_cast<unsigned>(blend.srcColorBlendFactor), static_cast<unsigned>(blend.dstColorBlendFactor),
                    pr341BlendConsts[0], pr341BlendConsts[1], pr341BlendConsts[2], pr341BlendConsts[3]);
                auto traceView{[](const char *role, size_t index, TextureView *view) {
                    if (index >= 32) return;
                    if (!view || !view->texture) { LOGI("PR341_AB_VIEW role={} index={} null", role, index); return; }
                    LOGI("PR341_AB_VIEW role={} index={} texture=0x{:X} backing={}x{} mips={} mip={}+{} layer={}+{} format={} swizzle={}/{}/{}/{}",
                        role, index, reinterpret_cast<uintptr_t>(view->texture.get()), view->texture->dimensions.width,
                        view->texture->dimensions.height, view->texture->levelCount, view->range.baseMipLevel, view->range.levelCount,
                        view->range.baseArrayLayer, view->range.layerCount, vk::to_string(view->format ? view->format->vkFormat : view->texture->format->vkFormat),
                        static_cast<unsigned>(view->mapping.r), static_cast<unsigned>(view->mapping.g),
                        static_cast<unsigned>(view->mapping.b), static_cast<unsigned>(view->mapping.a));
                }};
                size_t index{};
                for (auto *view : activeDescriptorSetSampledImages) traceView("sample", index++, view);
                index = 0;
                for (auto *view : activeState.GetColorAttachments()) traceView("target", index++, view);
            }
        }
'''
    result[DRAW] = replace_once(draw, anchor,
        '        vk::Rect2D scissor{GetDrawScissor()};\n' + probe + '\n        constantBuffers.ResetQuickBind();')
    return result


def load_original(root):
    original = {}
    for path, expected in EXPECTED.items():
        raw = (root / path).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f"Source is not the pinned master baseline (or was already prepared): {path}")
        original[path] = raw.decode()
    return original


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--variant", choices=VARIANTS, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root, out = args.source.resolve(), args.output.resolve()
    # Reject a different runtime baseline, even if selected source files happen to match.
    changed = subprocess.check_output(["git", "diff", BASE, "--name-only", "--", "app"], cwd=root, text=True)
    if changed.strip():
        raise ValueError("Runtime tree differs from pinned master: " + changed)
    original = load_original(root)
    generated = generate(original, args.variant)
    out.mkdir(parents=True, exist_ok=False)
    diff = ""
    for path, content in generated.items():
        before = original.get(path, "")
        diff += "".join(difflib.unified_diff(before.splitlines(True), content.splitlines(True),
                                          fromfile="a/" + path, tofile="b/" + path))
    (out / "runtime.diff").write_text(diff)
    manifest = {
        "experiment": "PR341 mip admission A/B; NOT a fix", "variant": args.variant, "base": BASE,
        "lab_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "common_probes": "512 bloom-manager events; 128 sampled draw reports, first 16 then 1/128",
        "source_before_sha256": EXPECTED,
        "source_after_sha256": {p: hashlib.sha256(c.encode()).hexdigest() for p, c in generated.items()},
        "diff_sha256": hashlib.sha256(diff.encode()).hexdigest(),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for path, content in generated.items():
        (root / path).write_text(content)
    print(json.dumps({"variant": args.variant, "manifest": str(out / "manifest.json"), "files": list(generated)}))


if __name__ == "__main__":
    main()
