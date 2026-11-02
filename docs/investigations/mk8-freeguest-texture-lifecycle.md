# MK8 initial black character models: temporary lifecycle trace

Base: master `ab2011a42f629a6c458ade02fb4104a6d249cb69`. No PR #400 code is imported or modified. ARM64 DevRelease configuration and workflows are unchanged. This branch changes observation only; it does not fix the black models.

## Evidence so far

`SubmitInternal` calls `SynchronizeHostInline(..., true)` for every attached texture. With Free Guest ON, even a sampled-only native image can transition CpuDirty/Clean -> GpuDirty and have its guest memory discarded. With Free Guest OFF, the existing `everUsedAsGpuWrite` guard prevents that transition. Existing ownership policy can deliberately keep a host-only copy; the classification alone does **not** prove lost bytes or the cause of the black models.

The host test in `tests/gpu_submission` runs the actual synchronization body and verifies both the original ON/OFF behavior and the ownership diagnostic. The device must establish which views are black, whether they were populated, and what changed after the race. No device run or three-phase result is claimed here.

## Moto G100 collection

1. Keep Free Guest Texture Memory ON, with the same driver and other settings as the failing run. Enable on-screen controls or the performance overlay. Start MK8 from a fresh process. Startup is phase 0; the recorder is enabled only for program NCA title `0100152000022000` with Free Guest ON.
2. **Before opening the first character selection**, long-press the controller visibility icon (or performance overlay). Select `1: First selection`. Enter selection and keep the same character/camera for 2–3 seconds. Take a screenshot/video showing the black model.
3. Before starting/loading the race, open the same diagnostic menu and select `2: Race`. Run a few seconds.
4. Before returning to character selection, select `3: Returned selection`. Return, choose the same character and keep the same camera for 2–3 seconds. Take a screenshot/video showing the correct model.
5. Long-press again; choose `Export trace and stop recording`. Save the CSV using Android's file picker. Send that CSV with the ordinary emulation log, screenshots/video, game version and driver. Export freezes the trace; restart emulation for another attempt. Phase markers are monotonic and repeated taps do not move their first boundary.

No ADB, log spam, GPU wait or readback is added for collection. Export copies the bounded trace on an IO worker, then formats/writes the CSV after releasing the recorder mutex. A phase/frame marker labels guest presentation requests, not a precise host display-completion timestamp.

## Schema and limits

Four independent budgets of 8,192 rows (startup + three phases), at most two examples per event/texture/phase, except up to sixteen mapping fragments. Record storage is allocated once before guest execution. Events do not allocate, format text, read texture bytes or perform I/O. Both recorder and state snapshots use try-lock; contention never waits. The trace is temporary and incurs bounded CPU/memory overhead; FPS impact still requires device measurement.

Every event includes a lifetime-unique texture ID, first guest CPU mapping address/size, initial guest format, current host format, dimensions, serials and flags. Separate Mapping rows enumerate fragments (index in detail). Mapping addresses are CPU mappings of guest memory, **not GPU IOVAs**. DescriptorIova has the TIC index in detail and GPU IOVA in peer; it is emitted during descriptor resolution, so cached TIC hits may reuse an earlier row.

Resource-locked snapshots additionally include image handle, backing generation, layout and fence-cycle identity. State-only trap/descriptor events set flag 16 and omit those resource fields. Metadata-only Mapping/View rows set flag 32; their dynamic fields are invalid. VkImage/alias/fence pointers can be reused; correlate with texture ID, generation and time. SampleRead/StorageRead/StorageWriteIntent rows include the active view format, aspect, mip/layer range, view type and packed swizzle (four 4-bit Vulkan component-swizzle enums, R in low bits). Storage events record the identity mapping actually selected by GetView(true). A binding event reflects descriptor preparation; cached descriptors may not generate a new event on every draw.

States: 0 Clean, 1 CpuDirty, 2 GpuDirty. Flags: 1 guest freed, 2 alias stale, 4 read-only alias, 8 retired, 16 state-only, 32 metadata-only. Ownership events have the previous state in detail. HostSyncRequest/InlineSyncRequest have the original requested gpuDirty boolean in detail. GuestSyncRequest detail: bit 0 cpuDirty, bit 1 skipTrap. ClearRequest detail: view aspects plus bit 16 for conditional rendering.

SampleRead/StorageRead/CopyRead are reads. RenderTargetIntent/StorageWriteIntent/CopyWriteIntent and WriteRecorded come from explicit write paths, independent of generic synchronization ownership. CopyFromRecorded denotes a submitted presentation/image copy. Intent/recorded commands are **not evidence that a conditional shader instruction actually executed**. UploadRecorded refers to an upload queued/recorded in the existing command path, not GPU completion. ReadbackComplete is emitted after the existing readback and guest encoding return. FreeGuest is emitted only after the existing FreeMemory call returns (not proof that the OS reclaimed every page).

The CSV reports contention, overflow, duplicate-limit and snapshot-miss counters separately per phase. Suppression retains only initial examples, so absence of a later event is inconclusive. `write_serial=0` at an ownership transition is direct cumulative evidence that RecordGpuWrite has not run on that texture yet; it still does not establish corrupted content.

NullIndex/NullFormat/NullMapping identify null-texture fallback reads explicitly, with TIC index in detail and IOVA in peer when available. Host-only textures receive IDs too.

Run `python3 tools/analyze_mk8_texture_trace.py trace.csv` for phase counts and candidate IDs. Compare identical IDs and overlapping mapping intervals, view metadata, FreeGuest, ReadbackComplete, AliasMember/AliasRecover and new texture creation across phases. Never equate a recycled address with a continuing texture lifetime.

## Targeted Mini RenderDoc fallback preparation

Do this only if the device lifecycle trace cannot establish the failing transition. This branch prepares the target/protocol; it does not automatically capture GPU image bytes or claim to have a failing/correct capture.

- Use the phase-1 and phase-3 screenshots to pick the same character pose/camera. Select candidate texture IDs/mapping intervals from the trace; identify their current VkImages and view ranges separately in each phase.
- Arm exactly one defective frame and one correct frame, with at most sixteen candidate sampled views plus their color/depth targets. Record draw order, shader hashes, TIC index/IOVA, exact view format/aspect/swizzle/mip/layer and binding/writer provenance. Include a presented-frame reference and the last producer of each candidate.
- Copy only the selected host subresources to dedicated staging buffers after their actual producers, preserve layouts and submission/lifetime dependencies, and never call SynchronizeGuest or republish guest memory to obtain the capture. No all-texture dump or continuous readback. A deliberately armed capture may stall and is excluded from FPS comparisons.
- Compare raw bytes and decoded channels for sampled inputs and the target before/after the character draw. If inputs match but outputs diverge, investigate shader/RT/depth state instead of changing Free Guest. If one candidate is already black/uninitialized, trace its last successful upload/producer and first eviction/recovery. Return to ordinary tracing after the two captures.

Actual Mini RenderDoc implementation/arming requires those candidate/frame targets from the first device run. The existing RenderDoc integration captures executor submissions, not automatically a whole presented frame; do not label one submission a complete frame capture.

## Verification

- `bash tests/mk8_texture_trace/run.sh`: bounded recorder, independent phases, deduplication, overflow, export/freeze and concurrent writers, ASan/UBSan (LeakSanitizer disabled in this host).
- `python3 tests/gpu_submission/run.py`: actual inline synchronization and submission/lifetime controls, UBSan.
- `python3 tests/gpu_sampled_layout/run.py`: layout round trips, aliases, retained TIC/retry controls, ASan/UBSan (LeakSanitizer disabled in this host).
- `python3 tests/gpu_conditional/run.py` and `python3 tests/gpu_draw_texture/run.py`: existing conditional-rendering/draw-texture controls. Host facades recognize diagnostic hooks; the conditional facade also recognizes the current master index-cache invalidation API.
- CSV analyzer checked against an export from the actual recorder.
- Full Vulkan backing-promotion suite requires Boost/Vulkan development headers and a Vulkan ICD; these are missing in this host. Local Android build also cannot download Gradle here. Existing PR CI remains the ARM64 DevRelease build authority.

## Empty Android export follow-up

The submitted ZIP contained a zero-byte CSV, including no header. That establishes an export failure, not absence of lifecycle events. Replace procfs descriptor reopening with direct writes to the borrowed document-provider fd, including interrupted/short write handling, and test a write-only pipe plus failure reporting. Export emits one row-count/status log per attempt so an unavailable recorder can be distinguished from a write failure. No texture lifecycle policy is changed. Device confirmation remains required.

## Targeted Mini input capture implementation

The first device CSV has 25,786 rows. Phase 2 saturates at presentation counter 2715, so it does not cover the subsequent race. IDs 273/873/885 are run-specific candidates, not permanent selectors. The new Mini recorder is independent of that CSV capacity. It selects native 32x32, six-layer B10G11R11 unsigned-float images from sampled bindings, at most eight distinct images for each manually armed window. It copies all backing mips/layers into dedicated staging before the next consuming draw, after prior queued producers and existing SubmitInternal uploads. It inserts no guest readback or dirty-state transition and preserves General layout with scoped transfer barriers. This is a candidate-input snapshot, not an entire RenderDoc frame; it does not capture shader binaries, RT/depth output or every draw. Presentation counter is a window label, not proof of a whole displayed frame boundary.

Capture ownership belongs to GPU, declared last so closures/staging are destroyed before device/allocator. JNI holds the owning OS/session (including DeviceState and GPU) alive during export. Export on the IO worker waits the producing cycle and rejects cancelled/unrecorded work; no wait is added during ordinary gameplay. Explicit capture can break a render pass, allocate staging and affect timing, so use ordinary runs to assess FPS. The cancelled flag observes fence cancellation only; it changes no cancellation or submission behavior.

### Collect the two inputs

1. Start a fresh MK8 session with Free Guest ON. Reach the black character selection.
2. Long-press the control visibility icon or FPS display. Choose **Mini: capture black selection now**. Keep the character visible for two seconds; if necessary change character and back to force bindings to refresh.
3. Enter a race, return to the now-correct selection, and use the same character/camera. Choose **Mini: capture correct selection now**; keep visible for two seconds.
4. Choose **Mini: export both captures**, save the `.mcap` file to Downloads while the game is running, and send it with screenshots and the ordinary log. Export Mini **before** exporting the lifecycle CSV, since that CSV action stops recording. Each side may be armed only once per session.

Both sides must have at least one candidate. An incomplete export reports failure and must not be treated as valid evidence. IDs and addresses may differ in another run; compare shape, TIC/view metadata, backing generation and decoded pixels. Eight images/side and 256 KiB/image bound the GPU allocations. The raw container begins `MK8MINI1`, then one numeric CSV metadata line per image, exactly `bytes` payload bytes, and one newline. The payload order follows GetBufferImageCopies: mip-major, six tightly packed array layers per mip. `tools/decode_mk8_mini.py input.mcap output_dir` verifies the framing/size, saves raw inputs, six-face contact sheets and numeric min/max/zero/nonfinite counts. Fixed display tonemapping is identical for both phases.

Host validation covers bounds/window/dedup/export framing, packed-float decoding and fence cancellation observation. Android compilation and real Vulkan image-copy validation remain CI/device checks; no corrected rendering or cause is claimed.

## Producer stage (MK8MINI2)

The uploaded `mk8-mini-1793639752677.mcap` contains four zeroed cubemap inputs in black selection and populated base mips in the same four images on return. Three already had nonzero write serials while black; no Free Guest correction follows from that alone.

New producer-only mode intercepts render-target subpasses (including helper blits) and color clears for the candidate 32x32 float cubemaps. At most eight operations per side are captured in the first eligible presentation-counter window. Each operation copies the target before/after and up to the first two eligible sampled inputs. Input copies require native-format, single-sample color 2D host images, at most 256x256, six layers and 256 KiB. Compressed native formats are intentionally retained as raw bytes. Other inputs are omitted; absence is inconclusive. Compute/storage-image producers and direct non-subpass copies are not intercepted by this stage. Conditional rendering remains enabled; an unchanged before/after pair is not proof the GPU executed a shader write.

Use a **fresh session**. On black selection select **Producer: capture black selection**, then change character and back to refresh work. After a race, on the corrected selection select **Producer: capture correct selection**, again refresh character and wait two seconds. Select the existing **Mini: export both captures**. Do not mix the old Mini arm buttons with producer mode, and export before stopping CSV recording. If no producer is observed, export fails rather than inventing a valid capture; report this with the log. Startup-only writers may require arming before entering selection on a follow-up run.

MK8MINI2 adds `operation,role` before the previous numeric metadata. Role 1=target before, 2=target after, 3=sampled input. Operation low bit identifies clear (1) vs general subpass (0). The old TIC column for producer inputs contains the sampled-array ordinal (first/second), not a TIC index. Target rows use zero. The frame and write serial describe CPU preparation, not GPU completion; packet order identifies recorded placement. All payloads use the existing queued upload/producer ordering, General layout and scoped barriers. Export rejects incomplete or changed-backing pairs. Use `tools/decode_mk8_mini.py` to retain raw payloads and metadata; 32x32 float cubes also receive previews, other source formats remain raw. Compare source zero-byte counts and target before/after per operation before choosing the next scope.


## Origin stage — automatic startup through returned selection

The prior captures show that the six-layer 32x32 R11G11B10 cubemap enters the
observed black-selection producers with all zero bytes, while the same backing
and generation has data after the race. **This is an observation-only diagnostic,
not a fix.** An independent, capped recorder is now activated at title startup
for MK8 (`0100152000022000`) only when Free Guest Texture Memory is ON.

This collector watches **shape plus CPU-mapping overlap**, not the runtime ID
244 or a hardcoded memory address: format B10G11R11_UFLOAT_PACK32, 32x32,
six layers, five mips, one guest mapping. It tracks up to eight distinct
mapping intervals and at most 320 evidence entries. Source textures overlapping
those intervals are in scope, even if their formats/dimensions differ.
Everything else is excluded. Non-blocking recorder contention and capacity
omissions are reported in the CSV header. Because the first cubemap appears
during startup, no manual arming is needed.

The CSV stages distinguish:
- `overlap_before/after`: mirror bytes around attempted synchronization of
  overlapping producers at new texture materialization. If memory had already
  been freed, a zero-sized metadata row is emitted, **not** a fake zero image.
- `created_guest`, `guest_before_upload`, `staging_before_upload`:
  actual guest mirror bytes and the CPU-prepared staging bytes. The latter is
  mip-major and can be compared by face/mip via separate CSV rows; block-linear
  guest bytes are **not** incorrectly decoded as linear images.
- `host_after_upload`: an extra image-to-buffer copy immediately after
  `CopyFromStagingBuffer` **within the same inline submission**, with the
  producing cycle waited for **only during export**. Independent uploads via
  `SynchronizeHost` are not covered by this particular host-after-upload hook.
- `before_free_guest/after_free_guest`: mirror bytes before the normal
  `FreeMemory` call and metadata afterward. The diagnostic does not change
  `MADV_REMOVE`, guest-memory policy, dirty flags or texture ownership. A
  `FreeGuest` event does not guarantee `MADV_REMOVE` succeeded.
- `write_intent`: writer path enum in `detail`, write serial and generation.
  This is **not** proof a conditional GPU write executed.
- `sampled_host`: at most two writer-serial-distinct host snapshots for each
  tracked cube in each selected phase, obtained before consuming draws. This can
  locate a first changed image during the race if it is subsequently sampled;
  it cannot observe every compute/storage producer or every frame.
- `readback_guest`: guest bytes after a successfully completed ordinary
  readback.

For CSV rows, `size=0` means metadata only, never a captured black texture.
`nonzero=0` is meaningful only when `size>0` and `status=ok`.
`invalid_deferred` denotes an image copy that could not be resolved, not
zero pixels. Hashes use FNV-1a 64-bit and are not a substitute for bytewise
shader-output analysis. A per-face/mip digest is emitted for native-format
linear staging and host buffers. IDs, pointers, frames and writer serials are
run-specific.

### One targeted device run

1. Start MK8 in a **fresh process** with Free Guest ON and the same Vulkan
   driver as the failing run. No Mini/Producer arm is required.
2. Before the first character selection, set phase `1: First selection`.
   Observe the black model; before the race starts, set `2: Race`.
3. Run the race briefly, then set `3: Returned selection` **before**
   returning to the selection screen. Observe the correctly rendered model.
4. While the game is still running, select
   **Origin: export automatic startup/race evidence** and save the CSV.
   Export disables this **origin-only** recorder; other diagnostic exports
   remain separately available. Send this new CSV alone initially.

This run tests whether guest/staging already contains zeros, whether an inline
Vulkan upload preserves supplied bytes, whether overlapping FreeGuest calls
precede missing guest bytes, and whether a later tracked GPU writer correlates
with the first nonzero host sample. It does **not** prove the system call succeeded,
that a render-target intent executed, or that the extra Vulkan readbacks have
no effect on timing. Never compare FPS from this capture run. It does not
replace the previous MK8MINI1/2 evidence and does not investigate NVDEC.

The instrumentation is temporary and should be removed before any real
performance comparison or production merge. No root cause is claimed.


### Focused readback overwrite probe (following first device origin CSV)

The first `MK8ORIGIN1` CSV contained 590 exported rows but only 320
internal entries, exhausting the **global** cap before race/return. On
the observed original cubemap mapping, one producer's 4,096-byte guest
readback at frame 3164 had 4,091 nonzero bytes, followed by the
overlapping 49,152-byte cubemap readback returning zero guest bytes;
other face readbacks followed. This is strong **ordering evidence**,
not yet proof that the second write overwrote the first producer's
physical pages, nor a proof of what caused the earlier black selection.

To make this falsifiable, the collector now reserves independent
entry budgets per manual phase: 192 startup, 384 black selection, 384
race and 192 returned selection. Its CSV header reports per-phase
counts and omissions; a saturated phase cannot consume another phase's
slots. No runtime ID or address is used for filtering.

On a structurally identified 32x32, six-layer, five-mip native
B10G11R11 cubemap, `SynchronizeGuest()` now records **directly**
around the pre-existing `CopyToGuest()` call:

- `guest_before_readback_write`: a complete snapshot of the existing
  guest mirror *immediately before* the normal host-to-guest encoding;
- `readback_source`: the 32,736-byte linear Vulkan readback source
  just before the encoding (no new GPU synchronization is requested);
- `guest_after_readback_write`: the same guest mirror *immediately
  after* the existing encoding, with no other emulator operation
  inserted between the two observations except the recording.

For both guest snapshots, per-face subrows include the 4,096-byte
block-linear mip0 and the first 1,024 bytes of mip1 at their measured
8,192-byte face stride. The surrounding block-linear padding is
represented in the complete 49,152-byte row, not misinterpreted as
pixels. In any stage, `size=0` denotes metadata-only and cannot be
used to conclude the texture was black.

**Interpretation:** if the *same readback call* changes a shared face
from nonzero to zero, then a destructive write is demonstrated at that
call site. It would still not explain why the cubemap was all-zero
before its initial upload; the first-selection issue may have more
than one contributing synchronization error. If before and after
remain zero, the earlier apparent overwrite ordering is not proved.

Sampled-host snapshots in phase 2 now allow up to 12 writer-serial-
distinct records per cube, rather than just two. They remain
nonblocking deferred GPU copies, not a continuous GPU trace; a producer
may be missed. The new native tests include per-phase overflow
isolation and before/source/after checks.

**Collection:** use the same fresh-launch sequence described above,
then export only **Origin: export automatic startup/race evidence**
*after reaching the corrected selection*. No Mini/Producer mode needed.
Do not change Free Guest or merge this diagnostic branch.
