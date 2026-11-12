# MK8 PR #341 — minimal bloom shader LOD check (diagnostic-only)

The historical master pair `381d52d3 -> 3e8689f6` isolates the introduction of the overbright pink/white bloom to #341. PR #447's descriptor-dependency fix did **not** visually resolve it.

## Evidence from device PR #448 (Moto G100, Turnip 24.0.99, Free Guest Texture Memory off)

| Arm | MIP_REUSE | Rejections | Visual |
| --- | ---: | ---: | --- |
| A_control | 472 | 0 | Excessive pink/white bloom |
| B_strict_tile | 181 | 291 | Major green/pink block corruption |
| C_base_mip_only | 72 | 400 | Major green/pink block corruption |

The first 512 bounded lookup events are counted; these are **not** total game events. In A the FS `05EB6F31815A3BF8` draws to individual mips 3, 2 and 1 of a 320x180, 5-mip `B10G11R11UfloatPack32` image, while its sampled image view covers mips 0 through 4 of **the same Vulkan image**. B/C split the sampled image from RTs, explaining why they can break dependency and appearance. All A FS draw reports in the bounded logging window relate to upsample; the compositor `F8F8901DE46A8AFF` was not sampled by A's draw budget. There are no PR341 promotion/reconcile probes in these logs; this does **not** exclude events outside the bounded sample.

**Unknown, decisive question:** does the fragment shader actually fetch the mip currently being written (feedback), or a disjoint mip? Broad image-view overlap alone does not prove a data race; Vulkan hazards depend on accessed subresources. Do not apply a forced LOD, brightness clamp, shader replacement or generic image copies without proving a real fault.

## This laboratory

Changes from master are limited to a hash-filtered, one-time **compiled SPIR-V export** in `ShaderManager::ProcessShaderBinary`, plus a distinct Android dev package `org.stratoemu.strato.mk8lod` and visible name `Strato MK8 LOD Lab`. The existing shader code and returned binary bytes are unmodified. Neither TexMan nor render pass logic is changed. The mainline PR CI assembles one ARM64 DevRelease APK. The two exact hashes are exported; other shaders are not dumped.

On phone, use the same MK8 build, stadium start and Turnip settings, with Free Guest Texture Memory off, and boot the **new lab app**. Consult `emulation.log` for `PR341_LOD_DUMP`, including the exact path (under `shader_dumps/<titleId>/` in app public files). Collect both `05EB6F31815A3BF8.spv` and `F8F8901DE46A8AFF.spv` into one ZIP and send for analysis. Android may restrict file browsing under Android/data; do not change code blindly if inaccessible. If a shader is loaded from an existing cache without recompilation, clear only the lab app cache, **not** the main Strato data.

Goal: inspect `OpImageSampleExplicitLod`, implicit LOD instructions and sampling operands, then correlate with the guest TSC clamps if needed. This is evidence gathering only; it does not fix the game and **must not be merged**.

## Update after first device SPIR-V collection

The supplied 05EB6F31815A3BF8 fragment SPIR-V has exactly **one OpImageSampleImplicitLod**, set 0 / binding 1, normalized `in_attr0.xy` coordinates and no explicit LOD, image fetch or gradients. RGB is scaled by uniform components 24–26; alpha is zero. The F8F8901DE46A8AFF compositor has three implicit texture samples, two 2D and one 3D LUT, and no explicit LOD. Consequently the TSC state and the vertex program's UV interpolation are the missing variables. This revision adds only:

- Export of vertex shader `C1625C567A83CB77.spv` in the same dump directory.
- Up to 48 `PR341_LOD_TSC` events for precisely the 320×180 five-mip HDR sampler bound by the `05EB6F31815A3BF8` fragment shader, recording chosen TSC index, bias, clamp and mip-filter mode, plus corresponding Vulkan min/max LOD.

**What to send:** launch the updated Strato MK8 LOD Lab APK, run MK8 Stadium through the start, and send the new `emulation.log` and `C1625C567A83CB77.spv`. The two existing fragment binaries need not be re-uploaded. If a shader cache prevents a new dump, see the application-specific dump path already in the log. No changes to TexMan, sampler creation, shader bytes or render passes. Not a visual fix; do not merge.
