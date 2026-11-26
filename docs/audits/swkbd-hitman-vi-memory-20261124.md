# Hitman inline SWKBD: VI image-memory / AM / VI audit

Parent: `a41f4ab91b13129e3f58a0bd20d7e5c281a0a46d`. Existing Draft PR #486, branch `fix/swkbd-lifecycle-overlay-20261124`. No merge, no new PR, no title-specific behaviour, touch controls and Android navigation untouched.

## Verdict

**The cause of the missing first Calc is not established, and nothing in the VI/AM path audited here can be shown to cause it.** The `0x3A0000` vs `0x3C0000` difference is real but is not a demonstrated blocker. One genuine conformance gap was found and fixed (IPC 2451 was unimplemented). A bounded, temporary guest-side trace was added so the next device log can say where the guest threads actually are.

## The supplied log

Same prefix as the previous log: mode `3`, API `0x8000D`, `InitializeArg=0x0000010000000000`, AM 160 (handle returned), `0xD`/`0xE` fully processed (state lock acquired/released, handlers returned), then VI `width=1280 height=720 required=0x3A0000 alignment=0x1000 result=0`. Everything from `create` to the VI reply happens within about 0.5 ms on HOS-12. After that there is no further `[SWKBD]` record from any thread. The log shows the VI handler *returned*; it cannot show that the guest consumed the reply, nor what the guest did afterwards (no SVC-level information existed).

## 1. The `0x20000` divergence

Strato computes `AlignUp(width*4*height, 0x20000)`, alignment `0x1000` (the yuzu/Eden formula; the yuzu commit that introduced 2460 uses the same `base_size = 0x20000`). The Ryujinx formula supplied in the task (pitch aligned to 64, height aligned to 64, then `0x1000`, then `0x20000`) differs only when padding the height to 64 crosses a granule:

| Size | image bytes | Strato | Ryujinx formula | delta |
| --- | --- | --- | --- | --- |
| 1280×720 | `0x384000` | `0x3A0000` | `0x3C0000` | `0x20000` |
| 1280×721 | `0x385400` | `0x3A0000` | `0x3C0000` | `0x20000` |
| 3840×2160 | `0x1FA4000` | `0x1FC0000` | `0x1FE0000` | `0x20000` |
| 1920×1080 | `0x7E9000` | `0x800000` | `0x800000` | 0 |
| 1366×768 | `0x400800` | `0x420000` | `0x420000` | 0 |

Why this is **not** shown to be causal:

- Coherence: the advertised size always covers the image Strato writes (`requiredSize >= stride*height`, at most one granule of slack) and ImageMap only demands `buffer >= imageSize`. Tested over 380 dimension pairs, and in a full flow where the guest allocates *exactly* the advertised size at the advertised alignment (guard bytes beyond the image stay untouched).
- Ordering: the query is a pre-Calc setup call. Calc carries flags/arguments (`0x4E8` bytes in this SDK), not this size. No evidence links the returned value to the decision to send Calc.
- Comparison runs: the user's PR #367 notes record that Eden, using the same formula, returned the same `0x3A0000`/`0x1000` and then reached repeated `GetIndirectLayerImageMap` calls with this title. That is a second-hand record in this repository (not re-verified here), but it is the strongest direct counter-evidence: the same value did not stop another emulator.
- Switchbrew ([Display services](https://switchbrew.org/wiki/Display_services)) gives no size/alignment formula for 2460, only "buffer size to use with the ImageMap commands" and "buffer address alignment".

Residual risk, stated plainly: if real firmware pads the layer to 64 rows and a guest sizes its own accesses from that padded height instead of using the returned size, Strato's smaller value would under-allocate by one granule. That is hypothetical, no evidence from this title supports it, Strato writes only `stride*height` bytes, and no ground-truth value from hardware is available. Per the task, the advertised value was therefore **not changed**; the difference is pinned by tests so a later decision is deliberate.

## 2. VI / AM audit findings

| Item | Result |
| --- | --- |
| 2460 IPC id `0x99C`, 2450 `0x992`; 2460 reply = two s64, size then alignment; no PID/ARUID | Correct. Now serialised by a shared helper with a wire-format test (a swapped order fails the test). |
| 2450 raw data s64 width, s64 height, u64 handle, u64 ARUID; reply s64 size, s64 stride | Correct, unchanged behaviour; argument order and consumed byte count are tested. |
| **2451 GetIndirectLayerImageCropMap (`0x993`)** | **Was not registered** (guest would get an unknown-command result). Implemented per Switchbrew: four packed f32, then the same four u64; shares the ImageMap path (the produced image is blank, so the crop cannot change content). Not claimed to be the Hitman cause: the records available for this title (PR #367: Eden's repeated `GetIndirectLayerImageMap`; SDK call to the 2460 query) point at the 2460/2450 family, not 2451. Layout of the u64 block is inferred from Switchbrew's note that 2450 calls the same function; unverified on hardware. |
| AM 160 | Stable handle while the layer lives; rejects wrong ARUID, missing PID, non-indirect modes and exited applets; handle invalidated on exit/terminate/destruction. Tested. |
| Validation order and lifetime (registry holds weak references, mutex released before calling the applet) | Verified, no inversion or leak found; closed/foreign handles and PID/ARUID mismatches rejected (tested). |
| Buffer validation | Size ≥ image, 0x1000 alignment, empty/short/misaligned buffers and overflowing dimensions rejected without writing a reply (tested). |
| Events | The applet state-changed event is only signalled by normal-mode completion, inline Finalize, RequestExit and Terminate (and reset on Start); nothing signals it before Calc, so it cannot make the SDK's exit-event check report an exit on this prefix. |
| Failures now log | The ImageMap path logs result and arguments on any failure (it previously only logged an unknown handle). |

## 3. SDK analysis

The ExeFS archive from the earlier work is **not available in this session**, so no new disassembly was performed. The earlier module-relative findings (see `swkbd-hitman-v2-20261124.md` and PR #367) are reused as is: `GetImageMemoryRequirement` (main `0x10D2DF4`, SDK `0x3CF254`) calls VI with 1280×720, then `Appear` (main `0x10D2E24`, SDK `0x3CD8A4`) only sets a flag; Calc (SDK `0x3CE2CC`) is sent from the game's update routine (main `0x10D3BC4`, only direct call site `0x10AA638`, guarded by branches at `0x10AA444`/`0x10AA62C`). Per those notes, on VI failure the SDK writes zeros to both outputs; Strato returns success. Whether the SDK validates the size/alignment, allocates, or what condition gates the update routine could not be re-derived here and remains open.

## 4. Changes

- `services/visrv/indirect_layer_ipc.h` (new): argument parsing for 2450/2451, reply serialisation for 2460/2450/2451 and the validate-and-render routine, shared by the VI service and the host tests.
- `services/visrv/IApplicationDisplayService.{h,cpp}`: uses the helpers; registers/implements 2451; failure logging; arms the temporary trace.
- `kernel/guest_boundary_trace.h` (new), `kernel/svc.cpp`, `services/base_service.cpp`: **temporary** diagnostic.
- Tests: `tests/swkbd/swkbd_tests.cpp`, `tests/swkbd/applet_protocol_tests.cpp`.

Unchanged: required-size policy, SWKBD protocol/state/replies, all previous fixes, Android code, touch controls.

## 5. Temporary guest trace (for the Moto G100 log)

When the VI memory-info reply is first produced, the trace arms and logs `[SWKBD] guest boundary trace armed on thread #N`. From then on, once per (guest thread, kind, object) it logs: `WaitSynchronization` (first handle; the object key distinguishes poll / finite / infinite waits; arg0 = handle count, arg1 = whether the first object was already signalled), `ArbitrateLock`, `WaitProcessWideKeyAtomic` and every service IPC command (`Class::Function`, command id). After 160 distinct records it logs one limit line and disarms (afterwards the cost is one relaxed atomic load). It does not change guest-visible behaviour, logs no game text, and is Info-level only. Reading it:

| What follows the VI reply | Meaning |
| --- | --- |
| Threads parked in `WaitSynchronization`/lock/condvar and no further AM/applet IPC | Find which handle/address the thread that should call Calc waits on (the AM accessor traces print its state-event handle for correlation). |
| Further IPC to other services but never AM `103` with Calc | The guest runs on, but its update routine does not reach Calc; continue guest-side (see the branches above). |
| `IsCompleted`/exit-event polls reporting signalled | Would explain a skipped Calc; none is expected from the code audit. |
| AM `103` with command `0xA` appears | Continue with the existing Calc/Appear/frontend trace. |

Remove it together with the other `[SWKBD]` diagnostics once the cause is established.

## 6. Validation

`bash tests/swkbd/run.sh` passes normally and under ASan/UBSan (leak detection disabled by the host's ptrace restriction); `git diff --check` is clean. New coverage:

- Layout: 380 dimension pairs (stride, image size, coverage, granularity, slack, alignment multiple, comparison with the Ryujinx formula), overflow and range edges (negative, zero, `INT64_MAX`, `INT64_MIN`, alignment round-up, 2³⁰ pixels).
- IPC: 2460/2450/2451 raw-data order, bytes consumed and reply field order/width (mutation-checked: swapping reply order or weakening the size check makes the tests fail).
- Flow, production accessor + registry + applet + the VI helpers: AM 160 → VI 2460 → allocate exactly the advertised size/alignment → ImageMap → Calc Initialize+Appear → FrontendOpened → Finalize, for 9 resolutions including 1280×720; rejection matrix (short/misaligned/empty buffer, overflow, zero/negative size, foreign PID/ARUID, unknown/closed handle, missing PID, wrong mode, applet not started → `NoData`).
- Trace: inert until armed, per-key dedupe, cap and self-disarm, concurrent duplicate emission.

## 7. Limits

- The Android native sources (`svc.cpp`, `base_service.cpp`, `IApplicationDisplayService.cpp`) cannot be compiled here (no NDK/network); they were reviewed by hand and the shared logic they now call is compiled and tested on the host. The CI build is the compile check; per request it is not monitored.
- The service object, SVC dispatch and guest scheduler are not exercised by the host harness.
- The Ryujinx commit link and Eden sources could not be fetched from this session; the Ryujinx formula is taken from the task text and the Eden formula from the PR #367 record.
- No hardware fix is claimed.

## Next collection

Same steps as the previous follow-up (APK from this commit, Log Level Info, fresh Hitman run, wait ~5 s at ENTER PROFILE NAME, exit normally, **Share Logs** immediately). Send the whole log; the lines after `guest boundary trace armed` are the new evidence.
