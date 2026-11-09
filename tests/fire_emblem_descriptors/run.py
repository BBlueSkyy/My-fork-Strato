#!/usr/bin/env python3
"""Regression contract for the Fire Emblem: Three Houses texel buffer descriptor crash.

Adreno proprietary 0.777.0 crashes inside vkUpdateDescriptorSets on the first graphics update whose single
UniformTexelBuffer write spans several bindings. The pipeline manager must write each texel buffer binding on its
own when the driver quirk is set, without touching any other driver or any other descriptor type.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SKYLINE = ROOT / "app/src/main/cpp/skyline"
GPU = SKYLINE / "gpu"
HERE = pathlib.Path(__file__).resolve().parent


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def require(ok, message):
    if not ok:
        fail(message)


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


# 1. Behavioural test of the production write planner
with tempfile.TemporaryDirectory(prefix="strato-fe3h-descriptors-") as directory:
    binary = pathlib.Path(directory) / "tests"
    sanitize = [] if os.environ.get("SANITIZE") == "0" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *sanitize,
                    f"-I{SKYLINE}", str(HERE / "tests.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# 2. The quirk is declared once and only enabled for the Qualcomm proprietary driver
traits_h = (GPU / "trait_manager.h").read_text()
traits_cpp = (GPU / "trait_manager.cpp").read_text()
require("bool needsIndividualTexelBufferBindingWrites{};" in traits_h, "Missing texel buffer binding write quirk")
require(traits_cpp.count("needsIndividualTexelBufferBindingWrites = true;") == 1, "Quirk must be enabled in exactly one driver case")
qualcomm = traits_cpp.split("case vk::DriverId::eQualcommProprietary:", 1)[1].split("case vk::DriverId::", 1)[0]
require("needsIndividualTexelBufferBindingWrites = true;" in qualcomm, "Quirk must only apply to the Qualcomm proprietary driver")
require("needsIndividualTexelBufferBindingWrites" in traits_cpp.split("QuirkManager::Summary()", 1)[1], "Quirk missing from the device summary")

# 3. The graphics pipeline manager sizes and emits texel buffer writes through the planner under that quirk
manager = (GPU / "interconnect/maxwell_3d/pipeline_manager.cpp").read_text()
require("common/descriptor_write_policy.h" in manager, "Graphics pipeline must use the shared descriptor write planner")
descriptor_info = function(manager, "static Pipeline::DescriptorInfo MakePipelineDescriptorInfo(")
require("DescriptorWriteCount(descs.size(), individualDescWrites)" in descriptor_info,
        "Write count must come from the same planner that emits the writes")
for stage_call in ("vk::DescriptorType::eUniformTexelBuffer", "vk::DescriptorType::eStorageTexelBuffer"):
    call = descriptor_info.split(f"pushBindings({stage_call}", 1)[1].split("pushBindings(", 1)[0]
    require("}, needsIndividualTexelBufferBindingWrites);" in call, f"{stage_call} write count ignores the texel buffer quirk")
require("gpu.traits.quirks.needsIndividualTexelBufferBindingWrites" in manager, "Pipeline construction must pass the quirk")

sync = function(manager, "DescriptorUpdateInfo *Pipeline::SyncDescriptors(")
require("PlanDescriptorWrites(descs, bindingIdx, needsIndividualTexelBufferBindingWrites" in sync, "Texel buffer writes must use the planner")
require(len(re.findall(r"ctx\.gpu\.traits\.quirks\.needsIndividualTexelBufferBindingWrites\);", sync)) == 2,
        "Both uniform and storage texel buffer writes must honor the quirk")
require("pTexelBufferView = &texelBufferDescs[texelBufferIdx + range.firstElement]" in sync,
        "Each write must point at its own slice of the flat view array")
# Other descriptor types keep their previous layout
require("writeBufferDescs(vk::DescriptorType::eUniformBuffer" in sync and "writeBufferDescs(vk::DescriptorType::eStorageBuffer" in sync,
        "Buffer descriptor writes must be untouched")

# 4. Temporary FE3H instrumentation is gone, release behaviour of the descriptor update is the original one
state_updater = (GPU / "interconnect/common/state_updater.h").read_text()
for token in ("FE3H", "SPLIT_MODE", "descriptorUpdateSequence", "isolateFe3hWrites"):
    require(token not in state_updater, f"Temporary instrumentation left in state_updater.h: {token}")
require("gpu.vkDevice.updateDescriptorSets(updateInfo->writes, {})" in state_updater, "Descriptor update call removed")
textures = (GPU / "interconnect/common/textures.cpp").read_text()
require("FE3H" not in textures, "Temporary 1D texture instrumentation left in textures.cpp")
buffer = (GPU / "buffer.cpp").read_text()
require("FE3H" not in buffer, "Temporary FE3H instrumentation left in buffer.cpp")

# 5. Texel buffer offsets are never rewritten, the alignment probe is Debug-only and diagnostic
texel_view = function(buffer, "vk::raii::BufferView *Buffer::GetTexelBufferView(")
require(".offset = offset, .range = size" in texel_view, "Texel buffer view offset must reach Vulkan untouched (shader texel indices depend on it)")
require("AlignUp" not in texel_view and "AlignDown" not in texel_view, "Texel buffer offsets must not be rounded")
require(texel_view.index("#ifndef NDEBUG") < texel_view.index("TEXEL-ALIGN") < texel_view.index("#endif"), "Alignment probe must be Debug-only")

# 6. Release-only Android performance build; the Vulkan layer stays behind
#    the original Debug-only guards and is not packaged into this APK.
gpu_cpp = (GPU.parent / "gpu.cpp").read_text()
instance = gpu_cpp.split("static vk::raii::Instance CreateInstance", 1)[1].split("static VkBool32 DebugCallback", 1)[0]
require("#ifndef NDEBUG" in instance, "Vulkan validation layer must stay Debug-only")
require('requiredLayers.push_back("VK_LAYER_KHRONOS_validation")' in instance, "Debug validation support removed")

workflow = (ROOT / ".github/workflows/pr_build.yml").read_text()
require("assembleDevRelease" in workflow, "Performance test must compile DevRelease")
require("assembleDevDebug" not in workflow, "Do not compile DevDebug in this performance run")
require("Install Khronos Validation Layer" not in workflow, "Do not install Validation Layer for release")
require("strato-pr${{ github.event.number }}-arm64-release.apk" in workflow, "Missing Release artifact")
require('"lib/arm64-v8a/libVkLayer_khronos_validation.so" not in libraries' in workflow,
        "Release APK must verify absence of the Validation Layer")

print("Fire Emblem texel buffer descriptor regression: PASS")
