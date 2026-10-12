# RE5: comparison of the new Docked good/bad captures

Analysis of the paired attachments supplied after the phase-4 publication in PR #337. **The rendering cause remains unproved.** A diagnostic coverage defect was identified and repaired in the same branch; no Texman, shader or synchronization correction is applied.

## Exact inputs and coverage

| Input | SHA-256 |
|---|---|
| `capture_001texturas quebradas.zip` | `46410250e4dbf7cd66a9a9b09cf6845a01df78eab2cc1d90c04a1a09234130f7` |
| `capture_001com texturas funcionando.zip` | `4688f0e7deb659fe72d6ca5acfa1a66833aad7df73f3fb1d6272057668ce9c38` |
| `emulation texturas quebradas.log` | `b32fb74a0d4c278f9dbe61b6625378c8338c20fad504e2c3e933fe9bb7762896` |
| `emulation com texturas funcionando.log` | `6e89d6f5f4ebcb8ab687c17d708473790cec203cdf0c140914df0339bd8c0b5d` |

Each ZIP has **12 files** (13 entries including the directory): four RGBA8 presentation raw images, four presentation metadata files, README, completion marker, event header and blit history. Each raw image is 1920x1080 / 8,294,400 bytes. Both event files are only 205 bytes of header: **zero SCENE_DRAW, scalar, draw-binding or intermediate-pass observations**. Neither log has a phase-2 or scalar-result message. The phase-4 journal proves this is a newer diagnostic collection, but the files do not include an exact APK commit fingerprint.

Both logs report Adreno 650, QualcommProprietary, Vulkan 1.1.276, driver 0.777.0 and Docked mode. They are separate process starts, not an uninterrupted paired capture: good loads 688 graphics pipelines from cache at startup, bad loads 38. This is a concrete confounder/history difference, not evidence that shader cache reuse causes or fixes RE5. Actual material shaders/descriptors are missing from these new files; the equal hashes in the older captures cannot substitute for them.

## What the pixels establish

All four bad presentation readbacks show dark characters and ground, while the good set shows textured characters/ground in the same area. Animation, UI overlays and timing differ, so a pixelwise difference is not a causal resource comparison. In bad frame zero, **775,657 pixels (37.4%)** have RGB `(0,7,7)`; in good frame zero, none do. This large, near-constant region is consistent with missing material/lighting data or zero-valued inputs, but it cannot identify a texture or constant.

The failure is already present in the **pre-swapchain GPU readback**. A fault limited to Android surface presentation/display cannot create these captured dark regions. Earlier rendering/postprocessing stages and sampled contents remain possible.

## Retained Fermi operation: no discriminating selection difference

There is one retained destination record in each bounded journal, with `evictions=0` and `pin_drops=0`. It is the latest operation for destination Texture ID 9, from source Texture ID 8 (operation IDs 16387 bad / 17093 good). Process-local identities and Vulkan addresses must not be equated across runs; compare the request/mapping/subresource metadata instead.

| Field | Bad and good |
|---|---|
| Fermi source/destination IOVA | `0x59A540000` → `0x59C7C0000` |
| Surface/request format | BGRA8 UNORM, color aspect |
| Dimensions, view | 1920x1080x1, 2D, mip 0+1, layer 0+1 |
| Tile/mapping | block height 16, depth 1, one 8,847,360-byte span |
| Mappings | source `0x1872D10000`, destination `0x1874F90000` |
| Rectangle/derivatives | full frame, source centered (0,0), du/dv = 1, point filter, no resolve/OOB |
| Texman selection | `route=full` for both resources |
| Returned Vulkan format/layout | BGRA8 UNORM, General, identity swizzle |
| Image flags | no MutableFormat needed for this native-format view |
| Source state | clean, memory not freed, never used as RT in the recorded state |
| Destination state | GPU dirty, memory not freed, used as RT |
| Descriptor backing | recorded creation backing equals current selected source backing within each run |
| Observed sync counts for tracked IDs 6/8/9 | one upload command each; zero downloads / guest-copy returns |
| Preparation/command | queued and vkCmdDraw recorded, not a GPU execution trace |

After removing process-specific handles/operation IDs, rejected candidate rows and differing candidate counts, **all other retained blit/sync lines are identical**. Bad scans 780/774 candidates for source/destination; good scans 1285/1277. The first two logged candidates are unrelated 960x540 FP16 textures whose spans do not contain the requested range. Their rejection is valid; their existence does not prove they are scene render targets. Additional loaded resources after another chapter are a possible reason for the larger cache, not a confirmed interpretation of those counts.

The presentation buffers cycle through translated spans beginning at `0x186E810000`, `0x186F0B0000`, and `0x186F950000`; each is 8,847,360 bytes. None overlaps the retained blit's source/destination span. A later shader could still consume the blit output, but **there is no captured binding to establish that join**.

The retained blit does not exercise the nonzero-mip fallback defect, foreign-format aliasing or a stale view/backing mismatch. This excludes those specific explanations for this one recorded operation only. Latest-per-destination storage is not the complete preparation/execution history and cannot clear all Texman paths. Recorded uploads and equal dirty states also cannot prove equal image contents or visibility at a shader load.

## Missing data and the collector defect

Before the follow-up, four paths had fixed 1280x720 gates:

- `SampleRe5SceneDraw` required depth and color at that size.
- `PrepareDiagnosticBindingProbe` required depth at that size.
- `PrepareDiagnosticScalarProbe` required depth at that size and one of two known fragment hashes.
- `TrackDiagnosticPassImages` required depth/color at that size.

The new files have no internal depth dimensions; presentation resolution alone does not establish them (the earlier capture already presented at 1920x1080 with 1280x720 scene targets). Consequently, **the exact reason the current scene probes are absent cannot be proved from these ZIPs**. Draw preparation could also have preceded the active four-presentation window because CPU/GPU/presentation timing is asynchronous. However, the fixed-size restriction is a reproduced collector defect: a matched 1920x1080 color/depth configuration is rejected by the old predicate. Its involvement in these empty collections is not yet demonstrated.

The follow-up fixes selection to nonzero matching color/depth backing dimensions with their respective aspects, preserving sampling/count/byte budgets. It also records up to 16 unique geometry configurations, even rejected ones, and reservation counts, so a further empty capture will expose a concrete eligibility reason. This is a diagnostic correction, not an RE5 rendering fix. No new scalar/image-copy mechanism or additional synchronization is introduced; the existing readbacks can now include eligible targets outside 720p.

## Hypotheses not established

- **Mip 1:** still a real static lookup divergence, not exercised by this retained blit and not sufficient to explain the user's failure on #244.
- **Zero scalar:** neither confirmed nor excluded; no scalar file is a coverage gap, never a zero value.
- **Wrong sampled texture/format/mip/backing:** absent material bindings prevent this comparison. The observed blit has no such mismatch, but cannot represent unobserved shader resources.
- **GPU synchronization:** recorded commands/state are not content checks or proof of consumer visibility; do not add broad waits.
- **Free Guest Texture Memory:** user disabled it; selected blit resources also report `memory_freed=false`. Nothing here implicates it.
- **Pipeline-cache history:** 38 versus 688 cached graphics pipelines is factual; absent per-draw pipeline/resource data prevents a causal conclusion. Do not erase caches and change multiple variables as a speculative fix.

## Smallest next experiment

Repeat the paired capture with the coverage-repaired #337 APK, keeping **Docked mode and driver unchanged**, preferably bad → 4-1 → return/good in one running process. The existing arming protocol is unchanged. The required new information is precisely: actual scene color/depth geometry, one material draw's real sampled descriptor/view/backing, its matching bounded Fermi history, and the preserved guest/GPU scalar observation.

Check for `SCENE_DRAW` and `draw_bindings_*.txt` before packaging both complete folders. If absent, include `SCENE_GEOMETRY` and the reservation-count header instead of treating missing probes as valid resources. Scalar output remains conditional on the selected shaders and descriptor provenance. Wait for late fence callbacks as documented. Only after a specific suspicious sampled subresource is joined to its producer should a content probe target that subresource; no rendering patch is justified by these ZIPs alone.
