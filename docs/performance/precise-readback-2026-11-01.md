# Preserve read authority across NCE faults

## Scope and recovered evidence

Base: master `fc8eb5196c198b281dce6bb3d9a9126b11ac5d4a`, including #409.
Shader compiler gitlink remains `a668a31370f1739fdbf5375cad3e171d0fbd0601`.
Drafts #400/#405/#406/#411 remain independent; none of their product changes
are included. Existing worktrees and #411 checkpoints were preserved.

The user reports about 30 FPS **with readback hacks enabled**, not a hack-free
result. #411 reduced synthetic CPU work but did not perceptibly improve Zelda.
#409 fixes SSBO inference and is not a demonstrated hack-free performance fix.
The historical cycle-order work already had an observational ESR parser and a
read-as-write buffer-origin repro. This change recovers/hardens that mechanism
for routing, rather than repeating the SSBO investigation. Its actual frequency
on the Moto G100 remains unknown.

## The structural cost and change

Both NCE signal paths passed `write=true` for every trapped access. An ordinary
CPU load from GPU-dirty memory therefore took Buffer/Texture's write callback.
The required readback was followed by CpuDirty ownership, so subsequent GPU use
uploaded unchanged data. A valid Clean buffer mirror, faulting because another resource imposes
PROT_NONE on its page, could also wait on an unrelated GPU reader because the handler treated the CPU load as a store.

The fix preserves the original access direction through the NCE fault boundary:
known userspace loads use the existing read callback, which retrieves mandatory
GPU data and keeps the resource Clean. Real stores still use the write callback,
preserve old bytes through readback when necessary and transition to CpuDirty.
No bytes are considered current by comparing their values or guessing usage.

There was a prerequisite bug in the previously dormant TrapManager read path:
a successful read callback changed an entry to WriteOnly but left `allNone`
true, allowing subsequent writes without a trap. Keeping it false retains the
write fault. This includes overlapping entries and partially completed retries.
Without this prerequisite, enabling read routing would be unsafe.

`common/fault_access.h` walks the bounded ARM64 Linux signal context records with
memcpy (no alignment assumptions, allocations, logging, PC reads or extra-context
pointer dereferences). A unique ESR, valid terminated record chain, user data
abort, SIGSEGV/SEGV_ACCERR and recognized translation/permission syndrome are
required. WnR selects load/store. Cache maintenance, table walks, invalid FAR,
missing/duplicate/truncated ESR or unknown syndromes retain legacy write routing.
The guest and host signal entry points use the same classifier. The JIT32 backend is unchanged.

An independent review found and corrected an initial eligibility error: ARM64
Linux PAGE_NONE clears PTE_VALID, so PROT_NONE traps can have **translation** FSC
0x04–0x07 despite SEGV_ACCERR. The initial permission-only matcher would have
kept the old path on those loads. A new RED/GREEN case covers this. MAPERR still
uses conservative routing. See primary sources below. Actual Moto kernel context
availability is pending; absent/unsupported records provide safety, not a gain.

## What the hacks save and violate

| Mechanism in master | Work bypassed | Dependency lost |
|---|---|---|
| Buffer staged write trap, Fast GPU Readback after accumulated wait threshold | Resource lock/fence completion before full backing->mirror memcpy; the memcpy itself remains | Producer may still write the backing. CPU can read stale/in-flight data; Fast Readback Writes can then reupload those bytes |
| Texture write trap, no aliases, threshold reached, memory not freed | Entire readback path: producer wait, download submission/wait, staging->guest conversion/copy | GPU authority is cleared without making guest bytes current |
| Fast Readback Writes | Chooses CpuDirty vs Clean after the unsynchronized buffer copy | Neither choice proves the copied bytes were complete; Clean can also discard subsequent CPU stores |

Production buffer/texture thresholds are cumulative waits (count threshold 6,
then measured time exceeding 250 ms), not a per-frame budget. Texture shortcut
is gated off for aliases and freed guest memory. Safe preparation loops wait
outside trapMutex/resource lock, re-check replacement cycles and retry; those
loops and all Vulkan dependencies are unchanged.

This change removes false write ownership, avoidable uploads and independent
reader waits. It does **not** omit the first GPU-dirty readback, narrow its range,
or make an actual CPU dependency asynchronous. The negative hack control shows
why its zero-wait latency cannot always be matched by a correct implementation.

## Measured local results

All numbers below are fixtures, not Zelda frames. Hacks are OFF in A and C.
The native transport uses host-coherent buffers and Vulkan synchronization
validation on llvmpipe (LLVM 20.1.2); this is not Turnip/Adreno.

| Controlled workload | A: safe legacy | C: safe precise |
|---|---:|---:|
| GPU write -> CPU load -> GPU read, 4096 B | 1 mandatory wait, 4096 B download, 4096 B false upload | 1 mandatory wait, 4096 B download, 0 B false upload |
| Clean mirror read with collateral PROT_NONE while GPU only reads backing | 1 unrelated completion wait | 0 waits; subsequent CPU store still waits |
| 100 native GPU fill/readback/partial-store/GPU-copy cycles | 100 mandatory waits; 409600 B downloaded; 409600 B false uploads | Same mandatory waits/download; 0 B false uploads |
| Native texture CPU read after render-target write, Free Guest ON/OFF | CpuDirty; 1 extra upload submission | Clean; 0 extra upload submissions; subsequent partial store uploads correctly |

Required uploads after actual CPU stores are excluded from the **false upload**
column and occur in both paths. Every output byte is compared. One native run's
wall times were 90,413 us A and 75,869 us C, including validation and all fixture
work; these are illustrative, not a stable GPU/CPU speedup benchmark or FPS
prediction. Counts/dependency outcomes are the reproducible evidence.
B (legacy buffer hack ON, Fast Readback Writes ON, threshold precondition met)
returns stale 0x77 instead of required 0x55 with zero waits. That is an expected
negative control, not an acceptable optimization.

## Validation executed

- ASan+UBSan: actual TrapManager/IntervalMap + both actual NCE routing expressions;
  permission and PROT_NONE translation reads/stores, RMW store direction,
  conservative unknowns, malformed/duplicate/unterminated contexts, malicious
  extra-context pointer, 20,000 bounds cases.
- ASan+UBSan: real shared memfd guest/mirror mappings and actual CPU load/store
  page faults, production staged Buffer callbacks/SyncGuest/SyncHost; complete
  and partial stores preserve other bytes, repeat reads, rearm, invalidation,
  expired resource, replaced pending cycles, and concurrent resource-lock retry.
  The contending owner can retrap while preparation waits, checking trapMutex
  is released before waiting on the resource.
- Partial aliases on one page, completed callback not repeated after another
  callback defers, removed alias and surviving store protection.
- ASan+UBSan native Vulkan real fill/copy/fence/readback, 100 A + 100 C cycles,
  synchronization validation, zero errors and exact data comparisons.
- UBSan native texture scope: real images/render-pass writes/readback/layout
  conversion, dense Free Guest ON/OFF, no read-only reupload, aliases/contention,
  failed readback recovery, registration after readback and read-only write gates;
  both image-format-list capabilities; zero Vulkan validation errors.
- Existing production FenceCycle/SubmitInternal tests under ASan+UBSan:
  pending submissions, cancellation, completion vs submission mutexes,
  predecessor lifetime/chaining and direct/staged buffer dependencies.
- Negative controls against unchanged master: read-protection and NCE routing
  fail with their specific expected assertions. Initial translation eligibility
  failure was separately reproduced before correcting it.

See `tests/readback_traps/README.md` for exact commands and facade boundaries.
LeakSanitizer alone is disabled due host /proc/ptrace limits. CPU native faults
are x86, with synthetic ARM64 ESR; an Android ARM64 build does not replace device
signal-context validation. No full-game, GPU timestamp or device trace captured.
The focused texture suite avoids unrelated historical padded Free Guest bridge
fixtures; it does not claim a clean full-suite run. The known clean no-op fixture
cancels its unused test command cycle, without bypassing a production fence.

## Eden principle and deferred alternatives

At the pinned local Eden revision 943f1fc, InvalidateNCE invalidates GPU cached
memory; the historically proposed readback-before-invalidation option (#4490)
addresses preserving GPU-produced bytes before CPU invalidation. Subsequent
revert discussion (#4584) shows why this cannot be treated as a universally
safe recipe. Strato uses protected guest pages, mirrors and per-resource cycles;
copying that option would not establish regional authority or ordering here.
The compatible principle is to make GPU bytes available **before CPU consumption**
and preserve the real access intent. Existing read callbacks do that; this fix
makes NCE reach them. No Eden code or option is copied.

Regional dirty tracking needs exact writer/reader footprints and cross-buffer /
texture alias authority. Current protection faults identify an access/page, not
the complete store footprint or every later CPU load. Skipping whole-resource
preservation on that information alone would lose partially overwritten bytes.
Not implemented without proof. Similarly, speculative asynchronous readbacks
could add submissions or just relocate the wait; no device overlap evidence
justifies shipping them. No new global waits, resource cache rewrites, hashes,
game exceptions, turbo policy, pacing or guest clock changes are included.

## Clocks, pacing and Moto G100 acceptance

ExecutionWaiterThread toggles adrenotools turbo around pending work. It does not
change the guest clock. Presentation converts requested guest timestamps to
CLOCK_MONOTONIC and applies swap interval; overlay FPS is not proof of simulation
speed. Whether the observed difference comes from governor ramp-up, actual GPU
execution, CPU waits or submission gaps requires device frequency/scheduling
traces. No governor diagnosis is claimed from the force-clock observation alone.

Use the same Link's Awakening 1.0.0 save/scene, Turnip 24.0.99, dock/resolution,
thermal start and duration on Moto G100. Initially Free Guest OFF; no 60-FPS
cheat. Compare A master+hacks OFF, B master+Fast GPU Readback ON (record Fast
Readback Writes state explicitly), and C candidate+both hacks OFF, all max clocks
OFF. Repeat in alternating order after warm-up, at least three samples.

Record presented frame intervals/median/p95/p99, real elapsed time vs identical
scripted game progress (or video with an external clock), flicker/corruption,
crashes and temperature. Do not equate FPS, requested presentation timestamps
and simulation speed. If tracing, collect GPU frequency, sched slices, submission
and fence waits from one common real-time interval, plus aggregated per-resource
readback/upload bytes and dirty transitions in a separate temporary diagnostic
build. Do not sum concurrent thread times and call that frame time. This clean
candidate has no new production instrumentation.

Then repeat C with max clocks ON to isolate frequency sensitivity (not as a
solution), and Free Guest ON/OFF for correctness. Validate Marvel Cosmic Invasion
(no magenta, buffers/shaders), SpongeBob Cosmic Shake (boot to menu, graphics),
Sparking! ZERO and other affected games for coherence/stability. No games were
run here. Normal simulation speed, no flicker, hack-free FPS gain, and closeness
to B remain pending device validation. Build/APK evidence belongs in the Draft PR.

## Primary references

- ARM64 signal ABI / inline ESR records:
  https://android.googlesource.com/kernel/common/+/0499a78369adacec1af29340b71ff8dd375b4697/arch/arm64/include/uapi/asm/sigcontext.h
- PAGE_NONE invalid PTE:
  https://android.googlesource.com/kernel/msm/+/f7fb80b48f802985ac40a9934520f3d0fbb0fcc4/arch/arm64/include/asm/pgtable-prot.h
- User ESR preservation, VMA access failure and SEGV_ACCERR:
  https://android.googlesource.com/kernel/common/+/de124365a7d2d/arch/arm64/mm/fault.c
- Syndrome bits:
  https://android.googlesource.com/kernel/common/+/8c51adb100d9453a35a66c6336d2ca33321673e7/arch/arm64/include/asm/esr.h
- Eden proposal and revert discussion:
  https://git.eden-emu.dev/eden-emu/eden/pulls/4490
  https://git.eden-emu.dev/eden-emu/eden/pulls/4584
