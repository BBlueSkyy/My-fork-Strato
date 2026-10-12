# Resident Evil 5 — selective scene mini-RenderDoc (phase 3: scalar provenance)

Draft PR #337, diagnostic only; **do not merge** as a rendering correction.

## Evidence from the actual gameplay capture

The RE5 gameplay capture contained four 1920x1080 pre-swapchain images, 36 color snapshots, and 2,388 sampled draw records; it exhausted 2,400 event slots in approximate presentation interval zero. Most snapshots were collected before the main 3D color target could be followed sufficiently.

Within those records:
- One 1280x720 color target was written by 886 draws and sampled another 1280x720 render target.
- The other 1280x720 target was written by 259 draws.
- Both use the same 1280x720 D24/S8 depth attachment.
- Frequently observed fragment hashes include `F3805B306B15F89A` and `B141A1171435D55E` on the main color path; `8E1C37D501C478E1` and `07C66B919FC55A45` on the upstream color path.

User confirmed the sky is normally fine; character models and parts of the ground are black. This is **not** proof those four shaders are defective, nor that an observed abnormal sky frame is a real rendering failure rather than a diagnostic timing side effect. No Title ID or pointer-specific rendering workaround is used.

## What changed

- Track post-pass color readbacks **only** from 1280x720 color images used with a 1280x720 depth attachment, not every sampled color texture or early postprocessing pass.
- Continue capturing source images immediately before Android swapchain copy.
- Record sparse `SCENE_DRAW` entries with shaders, sample textures, color/depth identity, scissor and blend state. Prefer four hashes found in the actual first capture, but still sample draws using other shaders to avoid falsely restricting the diagnosis.
- Per-approximate-present sampling: priority draws (first 8, then one per 32), other scene draws (first 4, then one per 128). This prevents the first busy interval from filling the event log.
- Budget: up to 6 pass readbacks per approximate present interval, up to 24 overall, 100 MiB of intermediate data, 2,400 total logged events. Capture frames remain four, with unique globally reserved snapshot IDs.
- Save depth attachment **identity**, not depth pixels. Phase 3 adds only buffer barriers restricted to the four probed bytes and their staging destination, outside the natural render pass; no pass splits, waits, guest writes or forced descriptor refreshes.

## How to capture

1. Install the diagnostic APK of the updated PR #337 after its build succeeds.
2. Enter gameplay when blackened characters and black ground patches are visible.
3. Rename `gpu_capture/re5_scene/ARM_CAPTURE.rename_this_file_when_ready` to `ARM_CAPTURE` in the Strato public app files. If the rename hint has not been created, enter gameplay and let a few presentations occur.
4. Resume gameplay and remain at the same location for 10–15 seconds.
5. Include the new `capture_###` folder, `gpu_events.txt`, `emulation.log`, a screenshot, and the Vulkan driver version. Wait for `MINIRD_RE5_PHASE2 wrote snapshot` and `MINIRD_RE5_SCALAR wrote probe` messages to stop before zipping; `capture_complete.txt` only reports that presentation acquisition finished.

## Interpretation and limitations

- The source image is read **pre-swapchain**. Compare it against post-pass color snapshots.
- `SCENE_DRAW` is sampled, not a full draw trace. A shader hash being present does not prove which pixels it affected.
- Snapshots are taken **at natural Vulkan render-pass ends**, not immediately before/after individual draw commands. Copying an image while a render pass is active is invalid without changing the rendering path; forcing pass splits is inappropriate for the first targeted probe.
- `present_index_approx` is not a strict GPU frame identifier. Use submission, render pass, texture and depth IDs to correlate.
- If upstream color is already wrong before sampling by a downstream shader, examine the upstream RT and its sampled/depth dependencies. If upstream looks right but downstream is black, inspect the downstream fragment shader input state or pipeline.
- First capture may be affected by GPU readback timing. Repeat only if results are inconsistent, not as a reflex.
- Once a **specific** problematic draw/pass is identified, add a temporary deeper probe in the same diagnostic branch if needed. Do not apply speculative Texman/shader/depth changes.

## Optional header-only host test

`g++ -std=c++20 -pthread -Wall -Wextra -Werror -I app/src/main/cpp/skyline tests/gpu/re5_scene_capture_tests.cpp -o /tmp/re5_scene_capture_tests && /tmp/re5_scene_capture_tests`

## Phase 3: smallest missing observation

The supplied PCHE v5 staging cache contains the guest instructions for the captured shaders. `F3805B306B15F89A` finishes RGB with three unconditional FMULs using **fragment stage 4, cbuf 3, byte offset 0x1C0**. The control shader `B141A1171435D55E` uses **stage 4, cbuf 4, byte offset 0** for its final RGB multiplier. Those dynamic values are absent from both supplied scene captures and the cache. A zero multiplier is a hypothesis, not a proven cause, and this diagnostic does not change either shader.

Each selected draw now produces `scalar_NNN.txt` and, when a valid descriptor was copied, a **four-byte** `scalar_NNN.raw`. Metadata includes:

- Unique probe ID, draw kind/count, shader tuple, approximate present, executor/submission/render-pass/subpass and color/depth identities.
- Guest raw mirror bits and float at draw preparation, guest mapping/view offset/size, GPU-dirty/direct-import/direct-shadow flags. Reading bypasses guest traps and does not synchronize or refresh either backing.
- Descriptor set, Vulkan binding and array element, origin (megabuffer or BufferView), actual VkBuffer/offset/range, byte offset of the GPU copy and resulting bits/float.
- Explicit statuses for unavailable descriptor provenance, missing guest mirror, invalid/unverifiable range, invalidation or file errors. Unknown data is never represented as zero.

Descriptor provenance is captured from the **same `VkDescriptorBufferInfo` populated by StateUpdater for the submitted descriptor writes**, including rolled-over uniform writes and quick-bind descriptor copies. Reused draws keep that reference; full updates, uncopied slots, flushes and non-graphics pipeline changes invalidate it. A new BufferView resolution or a freshly uploaded diagnostic constant buffer would not measure the descriptor that the draw used and is deliberately avoided.

Probes cover direct, indirect and inline-index draws. Registration happens after natural pass selection but before AddSubpass can submit/reset state. Copies execute at the natural pass end, before pending post-pass commands. Fence-ordered callbacks invalidate staging memory before saving and carry their own capture path, so finishing the presentation acquisition does not discard late GPU results. No render pass is split, no executor flush is added and no guest value/binding is changed.

Budget: first eight draws and then one per 32 for each of the two fragment hashes, independently in each approximate presentation interval; at most **32 probes per interval, 128 total, 512 bytes of scalar payload**. The existing image/event budgets remain in place. Hash selection affects diagnostic sampling only.

### What this can and cannot prove

The raw file observes bytes in the buffer identified by the draw's descriptor **at the natural pass end**, after its consumers, with a four-byte transfer dependency. It is not an execution trace of the shader's uniform load. Megabuffer allocations are snapshots retained for the executor cycle; BufferView backings can still be changed by writes or direct-memory activity. Even on megabuffers, correct bytes do not prove that the consumer had the correct visibility dependency. Guest mirror bytes are also an unsynchronized observation and may differ from the normal direct-shadow read path. These distinctions are included in every record.

Compare a bad and a good capture from the updated APK, preferably at the same camera/scene. If the actual buffer scalar differs, follow its producer and binding provenance before proposing a fix. If it is identical and nonzero in relevant draws, the zero-multiplier hypothesis loses support; sampled texture/view contents and depth remain candidates. An absence of probes is a capture coverage limitation, not evidence of a good value.

### Validation

- Host sampling/budget test and scalar range/provenance tests; ASan/UBSan (LeakSanitizer is unavailable under this container's process restrictions).
- `tests/gpu/re5_scalar_vulkan_tests.cpp` calls the **production** readback helper with pinned Vulkan-Hpp and the same compile definitions as Android. It checks zero/one values, a nonzero descriptor base plus 0x1C0, missing/short/whole/overflow ranges, exactly four copied bytes, and a subsequent GPU write. Run with a graphics-capable ICD and Khronos synchronization validation.
- Android integration compilation is delegated to the existing PR Build workflow on push. Host tests do not establish device behavior or the game's root cause.
