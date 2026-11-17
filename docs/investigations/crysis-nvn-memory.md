# Crysis 1.6.0 NVN memory investigation (PR #410)

Starting head: `59c83f19a461463810a28a265075b96897b71f3c`.
The branch was clean, Draft, unmerged, with all 25 existing commits retained.

## Authorized diagnostic plan

The user requests continuation on this branch, bounded title/version-specific
instrumentation if needed, ARM64 DevRelease, and no speculative compatibility fix.
Work remains in the dedicated checkout of that branch; no new branch is needed.

- [x] Inspect history, latest device log and matching update executables.
- [x] Resolve NVN functions in the update SDK, independently of the base NSOs.
- [x] Test all-or-nothing title/build/signature gating and state arithmetic.
- [x] Record bounded pre-call NVN lifecycle history and callback state without
  invoking guest functions from host C++, changing results or bypassing abort.
- [x] Verify replay of displaced instructions and preserve guest registers.
- [ ] Review and publish diagnostics on #410, await DevRelease CI and device log.

The small diagnostic implementation uses an isolated portable header for verified
probe installation and the SDK-derived layout, `nso.cpp` for Build ID gating, and
the existing NCE trampoline/handler for recording. Tests exercise invalid identity,
truncated or modified code, exact replacement boundaries, reserved trailer accounting,
and invalid cursor ordering. Existing NVN callback and abort diagnostics stay active.
No GPU, NVDRV, memory allocator, shader compiler or service semantics are changed.

## Evidence and unresolved questions

The supplied latest Strato log matches updated main Build ID
`FC23D4BC6361081733A45E27D81275209A370553` and SDK
`2F08862A30911F87A13B11F7DD0F509DCB422D2D` (remaining Build ID bytes zero).
At callback: event 0, minimum additional size 512, configured command/control
capacities 12 MiB / 512 KiB. Stored capacities alone do not prove actual consumption.

The supplied Eden log uses the **base** NSOs, including main `926553C6...` and SDK
`204EAEF8...`; it is not a same-version comparison. A matching update-1.6.0 Eden
capture is needed before claiming a runtime divergence for identical binaries.

Update SDK RVAs, resolved through the NVN procedure resolver and relocation table:

| Operation | RVA |
| --- | --- |
| AddCommandMemory | 0x45AD80 |
| AddControlMemory | 0x45AF40 |
| GetCommandMemorySize/Used/Free | 0x45B010 / 0x45B020 / 0x45B030 |
| GetControlMemorySize/Used/Free | 0x45B040 / 0x45B060 / 0x45B070 |
| BeginRecording / EndRecording | 0x45B080 / 0x45B120 |

For command-buffer pointer C, the actual getters calculate:
command size = `[C+0x20]-[C+0x50]`, used = `[C+0x18]-[C+0x50]`,
free = `[C+0x20]-[C+0x18]`. Control end is `[C+0x38]+16`,
start `[C+0x58]`, cursor `[C+0x30]`.
`AddCommandMemory` subtracts `4*u32[C+0x70]` reserved trailer bytes from
the usable end. `BeginRecording` saves the existing cursor as a segment boundary;
it does **not** reset command memory. This is SDK behavior, not a Strato bug.

The later AbortImpl x4 remains unattributed. The new measurements must read the
actual SDK cursor field instead of reporting x4 as a cursor.

Main's four-slot ring path at 0x284B474 rotates the slot, waits, re-adds command
memory at 0x284B4BC, re-adds control memory at 0x284B4D4 and begins recording at
0x284B4EC. Two alternate Begin sites also have preceding Adds. It is therefore
incorrect to infer a missing reset solely from BeginRecording not rewinding.

NVDRV `0xC010001E` registers an asynchronous event and returns TryAgain when the
fence is pending; Strato translates TryAgain to NvResult::Timeout (5). This result
alone is not proof of an elapsed host wait or a failed GPU. Eden's retry fallback
and GPU-side wait behavior differ, but no causal connection to this NVN callback
has been demonstrated. Do not port either difference speculatively.

Reference Eden source: official `https://git.eden-emu.dev/eden-emu/eden.git`,
commit `7a6d055094846b7787f9b8c0f238c0c68709f632` (not the exact captured nightly).
Strato `services/nvdrv/devices/nvhost/ctrl.cpp` and `driver.cpp` were compared
with Eden `src/core/hle/service/nvdrv/devices/nvhost_ctrl.cpp`:
Eden counts cancelled waits and after repeated failures performs WaitHost;
Strato does not. Strato GPFIFO Wait enforces a host threshold wait, whereas
Eden's Puller Acquire has its WaitFence call commented out. SubmitGpfifo fence
increment arithmetic agrees. These code differences do not establish the cause.

## Verification and test interpretation

- Portable C++ tests: title/module/full 32-byte Build ID rejection, truncated
  signature rejection without partial mutation, exact replaced bytes and replay
  opcodes, used/free accounting including reserved trailer, invalid cursor order
  and control-end overflow. Normal and ASan/UBSan runs passed; LeakSanitizer is
  unavailable under this environment's ptrace, so `detect_leaks=0` was used.
- `tests/crysis_nvn/run_trampoline.py` compiles the actual production
  GetPatchData/PatchCode emitters and assembles actual `nce/guest.S`. Unicorn
  executes all five diagnostic stubs with four NZCV values, while a host-handler
  boundary clobbers caller-saved registers, SIMD and FP state. Guest registers,
  TLS, flags, exact stack effects, displaced stores, LR/SP recovery, branch
  destinations and patch-buffer canaries passed.
- This test exposed **pre-existing** SaveCtx stores of FPSR/FPCR at 0x298/0x29C
  overlapping Q31's upper half. It failed before the diagnostic stubs saved Q31
  separately. Only synthetic diagnostic stubs now push/pop Q31 around the legacy
  context helpers (10 words instead of 8). Ordinary SVC behavior is unchanged and
  is explicitly characterized by the test; no Crysis root cause is claimed from
  this unrelated observation. Original guest SP is savedSp+32 for these probes.
- Existing NSO, NPDM, HID and GPU submission host suites passed. Fresh read-only
  review found no important/critical issue in the initial incremental diagnostic
  change; the subsequent Q31 protection passed the ARM64 execution test.

No functional GPU/NVDRV correction is being published: actual callback used/free,
last Add/Begin/End sequence, emitting API caller and full CPU backing readability
still require the next device run. CPU backing checks do not validate GPU VA PTEs.
Pre-call snapshots must not be mistaken for post-call results or a validated unwind.

Expected device markers: `verified update SDK`, `CrysisNVNUsage`, `CrysisNVNCursors`,
`CrysisNVNCall`, `CrysisNVNHistory` through its END marker, `CrysisNVNBacking`,
`CrysisNVNTail`, and the preserved original abort diagnostics. The tail is capped
at 256 bytes; history at 64 operations. Host history mutex protects only diagnostic
records; it does not change or serialize guest NVN calls themselves.
