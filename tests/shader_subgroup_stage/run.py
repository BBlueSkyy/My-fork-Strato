#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"
COMPILER = ROOT / "app/libraries/shader-compiler"

traits_h = (GPU / "trait_manager.h").read_text()
traits_cpp = (GPU / "trait_manager.cpp").read_text()
manager = (GPU / "shader_manager.cpp").read_text()
profile = (COMPILER / "profile.h").read_text()
emit = (COMPILER / "backend/spirv/emit_spirv.cpp").read_text()
ctx = (COMPILER / "backend/spirv/spirv_emit_context.cpp").read_text()
warp = (COMPILER / "backend/spirv/emit_spirv_warp.cpp").read_text()

checks = [
    ("Strato traits", "subgroupSupportedStages", traits_h),
    ("Vulkan property", "subgroupSupportedStages = subgroupProperties.supportedStages", traits_cpp),
    ("stage mapping", "vk::ShaderStageFlagBits::eFragment", manager),
    ("profile mapping", ".supported_subgroup_stages = supportedSubgroupStages", manager),
    ("profile helper", "SupportsSubgroupStage", profile),
    ("capability gate", "profile.SupportsSubgroupStage(ctx.stage)", emit),
    ("builtin gate", "profile.SupportsSubgroupStage(stage)", ctx),
    ("warp fallback", "StageSupportsSubgroups", warp),
]
for area, token, source in checks:
    if token not in source:
        raise SystemExit(f"missing subgroup stage support in {area}: {token}")

print("Strato subgroup stage contract: ok")
