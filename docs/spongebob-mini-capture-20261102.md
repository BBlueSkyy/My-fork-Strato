# SpongeBob: The Cosmic Shake — natural render-pass trace (PR Draft #421)

## Why this diagnostic exists

- Earlier SBMINI1 captured NaNs in a 1280×720 Vulkan HDR R11G11B10 color target: 140 pixels in boot 1 and 273 in boot 2, **after an entire render pass**.
- SBMINI2 isolated **different** first eligible draws between boots, with different shader hashes and 0 changed pixels/NaNs in the measured 64×64 center ROI. Its forced FinishRenderPass/Submit may have hidden or changed the earlier effect.
- The root cause is **not proven**. This diagnostic preserves natural render-pass boundaries while recording a bounded sequence of shader identities from the measured pass.

## Capturing on Android

1. Install a newly successful ARM64 DevRelease APK from the existing Draft #421. Previous versions of the APK cannot export SBMINI3.
2. Use the same Turnip 24.0.99 driver and game version. Begin with Free Guest Texture Memory OFF.
3. Enter the graphically corrupted SpongeBob menu and leave emulation **running**.
4. Long-press the virtual-controller visibility icon, or long-press the enabled FPS overlay.
5. Select **Capture next scene/HDR pass (recommended)**. The arming persists until the **beginning of a naturally new** eligible render pass (target 2D color, >=320×240, Vulkan GENERAL, transfer-source capable, and at least one eligible sampled input) whose output is HDR R11G11B10 or whose sampled inputs include an image at least 16 pixels wide or high. This skips common overlays that sample only 1×1 white textures. If no capture is found, the same menu has **Capture any render pass (fallback)**, which restores the former unfiltered selection.
6. Leave emulation running. The selected render pass is **not artificially ended**. It may span multiple draws. Wait until it naturally finishes/flushes.
7. Return to the same menu, select **Export GPU Mini capture**, and save `spongebob-mini-*.mcap` using the Android file picker.
8. Save `emulation.log` with the file. Both are needed: the binary contains target/input ROI and the initial pipeline; the log contains the **bounded draw sequence for the naturally closed pass**.
9. Close the emulator fully and repeat with identical settings for boot 2. Optionally arm twice more before export if the first measured pass has no NaNs.

If export says incomplete, the selected pass may not have naturally ended or its GPU fence may not yet be complete; leave emulation running and retry. A 0-byte file is not a valid capture. A pass that does not naturally end cannot be exported as a valid pass capture.

## SBMINI3 semantics

- Custom binary format, **not** a native RenderDoc `.rdc`.
- **Role 1**: 64×64 center ROI of color target immediately before starting an eligible new render pass.
- **Roles 3–6**: up to four eligible inputs (top-left ROI), captured before the pass. These belong to the **first selected draw only**. The updated selector prioritizes nontrivial sampled images within these four slots (while keeping the descriptor ordinal) instead of filling them with 1×1 overlays.
- **Role 2**: same color target ROI **after the natural end of the entire render pass**, including its later draws, clears and other work. This snapshot does not identify a single responsible draw.
- Pre-copy executes when no RP is active; post-copy is deferred until the render-pass completion nodes. There is **no diagnostic call to FinishRenderPass or Submit**, and no forced render-pass split.
- An explicitly armed capture adds copy commands/barriers and can change timing; an ordinary disarmed run does not add GPU copy commands.
- As before, each ROI is <=64×64 and <=64 KiB. Max 3 operations and 6 image snapshots per operation; session export is bounded.
- The first selected draw metadata include shader hashes, pipeline XXH64, channel sequence, draw kind/topology/params, TIC cache ordinal and IOVA (if resolvable). These identifiers alone do not prove correct memory contents.
- Log `[SB_DRAW]` and `[SB_BIND]` identify the first draw and its sampled images; `[SB_PASS_DRAW]` records the following draw identities (first 128 events and subsequent power-of-two checkpoints). `[SB_PASS_NON_SHADER]` marks subpass operations lacking Maxwell shader metadata; `[SB_PASS_END]` gives total observed subpass events and natural-end or split reason.
- The RP identity in the logs is local to that emulation run. Comparing Vulkan handles or GPU pointer values between boots is invalid.

## Decode and interpret

```bash
python3 tools/decode_spongebob_mini.py boot1.mcap decoded1/
python3 tools/decode_spongebob_mini.py boot2.mcap decoded2/
python3 tools/compare_spongebob_mini.py boot1.mcap boot2.mcap
```

The decoder accepts legacy SBMINI1 (whole RP without shader identity), SBMINI2 (draw artificially isolated) and **SBMINI3 (naturally ended RP)**. A `capture_scope=natural_render_pass` manifest entry is required for the new analysis. The target's `target_new_nan_pixels` is a **whole-pass** delta, never proof that the first draw wrote the NaNs.

Compare two boots only when pass and target characteristics are reasonably matched; a shared first draw hash is helpful but insufficient to prove identical pass contents. Compare `[SB_PASS_DRAW]` sequences and scissor/conditional/depth state. If no matching pass is captured, the evidence is inconclusive.

**Next scientific decision:** When a natural pass produces NaNs, identify a reproducible shader/operation range in that pass before installing new instrumentation to bisect offending draws. Distinguish a resource-content failure from shader/descriptor/buffer errors using specific evidence, not guessed TextureManager changes.

## Why the capture selector changed

On the latest test, `spongebob-mini-1793679744190.mcap` was valid SBMINI3 (33,326 bytes). It selected a 1280×720 RGBA8 target, four draws, and a single sampled white 1×1 image (TIC 256). The ROI was already colored and **before/after were byte-identical**. The user had armed the capture during the final loading screen; the problem was not the user timing but the overly broad "first eligible pass" rule.

New **scene/HDR preference**: skip passes whose target is *not* R11G11B10 HDR and whose sampled inputs are all tiny (under 16 pixels in both dimensions). A bounded skip counter prints the first 12 candidates then powers of two via `[SB_MINI] skipped_overlay_pass=...`. The scene preference is not a full producer finder: a scene draw could legitimately use only small textures or buffers, and some composition passes have large inputs. An unfiltered fallback remains available, and captures still use the same SBMINI3 output format. No new GPU commands, memory behavior, ownership decisions or pass splitting are introduced in the disarmed path.

**Do not claim that skipped passes are irrelevant to the defect; this is only a capture prioritization heuristic.**

## Candidate writer provenance and truncated image buffers (next build)

The scene/HDR capture has now shown a **1920×1080 RGBA8 render pass with eight draws** that changes the measured 64×64 ROI by exactly a **0.6 RGB multiplier**, preserving alpha. That is consistent with a composition/fade pass, not necessarily the creator of the earlier pink/green/purple scene. The initial image is already colored. A producer trace is preferable to repeatedly capturing this same compositing pass.

### `[SB_PRODUCER]`: prior writer candidates

**Why this changed:** The next SBMINI3 capture `1793706730048` selected a composition pass with 8 draws and exact 158/255 RGB scaling, not the initial scene producer. Its prior global history logged `history_total=1111`, but a fixed 256-event ring had already discarded the earliest 855 candidates. The target still had two matching candidates with zero shader hashes; no `[SB_IMAGEBUF]` warning occurred in that boot. Repeating the same capture without improving provenance would not establish a cause.

After an explicit arm, the executor now stores summaries for **up to 512 different writer resource identities** (Texture object plus backing generation, or guest-address base for DMA). Each identity retains its **first observed writer, latest writer, first shader-identified writer, latest shader-identified writer, total candidate count, and identified-shader count**, even after thousands of later draws/clears to the *same* resource. Only when more than 512 **distinct resources** appear do the oldest resource summaries get evicted. The log reports `history_total`, `tracked_resources`, `evicted_resources` and each role's `writerCandidates`. This avoids the previous loss at 256 *events*; it is **not** an unbounded history.

At selection of the next scene/HDR pass, `[SB_PRODUCER] role=...` reports the earliest/latest candidate operation, kind and render-pass index, and the last shader-identified candidate's pipeline/VS/FS/CS. `[SB_PRODUCER_SOURCE]` reports the first shader-identified candidate, including channel sequence. `exact=1` means the same Texture pointer plus backing generation (process-local, not reliable across different boots); `exact=0` means possible overlap of the **first guest memory mapping**. `evicted>0` means some distinct-resource histories were discarded, not necessarily the target's; `writerCandidates=0` means only that no writer matched **within the explicitly armed, bounded window**.

**Source kind IDs:** 1=graphics draw, 2=storage image from graphics, 3=color clear, 4=compute storage image, 5=Fermi2D blit, 6=unclassified subpass, 7=Maxwell3D clear/helper, 8=depth clear, 9=Maxwell DMA copy, 10=Maxwell DMA fill. A known clear/blit/DMA kind with zero guest shader hashes is **expected**, not a shader hash collection failure. The Maxwell3D draw paths now attach pipeline/shader metadata for **all draws while armed**, even those inside an open render pass, while avoiding expensive per-draw TIC cache scans except at the start of an eligible natural pass.

All these are **possible writers**; conditional rendering, scissor, depth/stencil, zero-fragment draws, resource aliases and GPU scheduling can affect whether the target pixels were written. DMA records guest buffer destination spans and only provide *physical guest overlap hints* with texture memory; they are not automatically GPU image writes. CPU uploads, some synchronization/alias paths and writes **before the user armed** remain outside this history. The tracking does **not** perform GPU readbacks or force render-pass boundaries, does not modify descriptors, guest memory or resource authority, and is inactive outside an armed diagnostic window. This is not a complete dependency graph.

### `[SB_IMAGEBUF]`: image/texel-buffer binding truncation

The earlier log independently contained **16** `Split buffer mappings are not supported` warnings for `Textures::GetImageBufferView()`, with address `0x403FD0000`, requested `0x3FC000` and a smaller first mapped portion `0x200000`. Those warnings by themselves do not connect the buffer to the visible corruption.

Without changing `CachedMappedBufferView::Update()` or its fallback, the new diagnostic prints `[SB_IMAGEBUF]` when **the actual view size is smaller than the requested byte count**, including TIC index, IOVA, shader image format, Vulkan format, pipeline shader hashes, pipeline stage (including compute), read/write access mode, requested bytes, bound bytes and missing bytes. First 24 occurrences and later powers of two are logged. These messages can occur before the manual capture is armed and help identify shader users during loading.

**A short binding does not prove the shader actually reads the missing portion.** No speculative change to mapping length, buffer permissions, GPU dirty state, descriptor contents or the sequencing PR #400 is justified by these warnings alone.

### Recommended next test

1. Install the newly **successful** ARM64 DevRelease build of Draft #421.
2. Keep Turnip 24.0.99, Free Guest OFF, and do not clear shader cache between boots.
3. On the last loading screen before the title menu, arm **Capture next scene/HDR pass (recommended)**.
4. Let the game continue into the corrupted menu. Export one `SBMINI3 .mcap` with the complete `emulation.log`. Capture a screenshot of the title menu.
5. Repeat on another full boot; compare `SB_PRODUCER`, `SB_IMAGEBUF`, `SB_PASS_DRAW` and target bytes. If the filter never selects a pass, capture in the **unfiltered fallback** mode on a separate boot. Do not keep repeating an unchanged capture blindly.

**What this can establish:** first/latest candidates for the same texture across thousands of draws, source operation type, first/last identifiable guest shaders, possible compute storage-image writers and buffer-transfer overlap hints; together with SBMINI3 pixels and the independent image-buffer extent warnings, this narrows the producer chain. **What it cannot yet establish:** that any candidate wrote the exact corrupt pixels or that truncated bindings caused the graphics defect. The next step, if needed, is a narrowly targeted first-writer pixel snapshot for an identified target — not a speculative TextureManager or GPU sequencing rewrite.

## Code and validation

The changes are exclusively in the existing PR Draft #421. The resource sequencing branch/PR #400 remains unchanged. Never merge on the basis of a diagnostic capture alone.

`bash tests/spongebob_mini/run.sh` includes host controller/decoder tests with UBSan. These tests and Android CI must pass before treating the capture as validated. Real-device correctness of Vulkan snapshots must be confirmed by looking at the emitted files and logs. Previous SBMINI1/2 files remain useful but **do not mean the same thing** as SBMINI3.
