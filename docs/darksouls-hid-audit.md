# Dark Souls Remastered: HID consumer and ARM64 context audit

Base: `BBlueSkyy/My-fork-Strato` master
`8da63a2ae3f48c3acbcaf542128030fb298609bf` (2026-10-10 audit).

## Evidence and scope

The supplied device results are negative results, not proof of a particular
consumer: #230 did not fix walking; #231 recorded neutral producer samples but
its page faults were an Npad header and FullKey SixAxis header; #232 changed
direction without fixing the problem; #325 did not fix it; #326 still walked
with all published Npad sticks and derived stick-direction bits neutral.
#327 is cancelled and is neither reused nor continued here.

This audit found a reproducible ARM64 context defect. Its relationship to the
Dark Souls symptom remains **unproven**. No device reproduction, SDK binary
disassembly or successful live SDK-hook capture is available in this task.

## Pinned reference implementations

| Project | Inspected revision | Files |
|---|---|---|
| Strato fork | `8da63a2ae3f48c3acbcaf542128030fb298609bf` | `input.cpp`, `input/npad{,_device}.{h,cpp}`, `input/sections/{common,Npad}.h`, `IHidServer.cpp`, `K{Shared,}Memory.cpp`, `nce/{guest.h,guest.S}`, `nce.cpp` |
| Eden mirror | `67bada77f8a43a90da2e94e89b8e7da73c256989` | `src/hid_core/resources/{ring_lifo.h,npad/npad.cpp,npad/npad_types.h}`, `src/core/arm/nce/{guest_context.h,patcher.cpp}` |
| Yuzu mainline archive | `7ffac53c9e67fa18d067114a8eb3e77c74bba68f` | same HID/NCE paths as Eden |
| Ryujinx archive | `534f92506bf8c6aba29bb86917bf10954a360d8e` | `src/Ryujinx.HLE/HOS/Services/Hid/{HidDevices/NpadDevices.cs,Types/SharedMemory/Common/{RingLifo,AtomicStorage}.cs,Types/SharedMemory/Npad/*}`, `src/ARMeilleure/State/NativeContext.cs` |
| libnx | `feebd026ca0f5dcc2119f46ad8e0d16ad3dd4973` | `nx/source/services/hid.c`, `nx/include/switch/services/hid.h` |

Sources: [Eden](https://github.com/eden-emulator/mirror),
[Yuzu archive](https://github.com/yuzu-emu-mirror/yuzu-mainline),
[Ryujinx archive](https://github.com/kotx/Ryujinx),
[libnx](https://github.com/switchbrew/libnx).
The libnx reader is reference source, **not the Dark Souls SDK implementation**.

## RingLifo and region selection

Npad entries start at HID offset `0x9A00`, stride `0x5000`. IDs 0–7 map to
slots 0–7; Handheld `0x20` maps to slot 8; Other `0x10` maps to slot 9.
These mappings agree across the inspected implementations.

| Ring index in diagnostic | Offset inside Npad | Strato path |
|---|---|---|
| 0 | `0x28` | FullKey (also GameCube common state) |
| 1 | `0x378` | Handheld |
| 2 | `0x6C8` | JoyDual |
| 3 | `0xA18` | JoyLeft |
| 4 | `0xD68` | JoyRight |
| 5 | `0x10B8` | Palma; older SDK declarations name this position System |
| 6 | `0x1408` | SystemExt (`defaultController`) |

Each ring is `0x350` bytes: a `0x20`-byte header followed by 17 atomic-storage
entries, each `0x30` bytes. Header fields are timestamp/unused at +0, capacity
at +8, tail at +0x10 and valid count at +0x18. Strato's names `entryCount` and
`maxEntry` obscure the latter two meanings but their current values are
capacity 17 and count capped at 16. Construction writes 19 empty entries,
matching the initialization pattern in Yuzu/Eden.

Storage has its marker at +0, payload sequence at +8, buttons at +0x10,
LX/LY/RX/RY at +0x18/+0x1C/+0x20/+0x24, and connection attributes at +0x28.
The public SDK/libnx output omits the outer marker and is `0x28` bytes. libnx
describes attributes as u32 plus reserved u32; Strato uses a u64 with the same
low connection bits. This does not shift the sticks.

The inspected libnx consumer reads count/tail with acquire loads, copies
payloads oldest-to-newest into a newest-first output array, checks the outer
marker before/after each copy, and checks consecutive inner sequences differ
by one. It retries on mismatch. It does not use the first header word or
require the marker to equal the inner sequence. Strato increments each ring's
inner sequence by one and uses a completed marker of twice that value.
Current Eden also uses the doubled marker; archived Yuzu and Ryujinx differ.
Thus marker magnitude alone is not an established rejection mechanism.
Strato's temporary odd marker is not sufficient to prove a seqlock contract:
the inspected libnx reader checks equality, not parity.

Strato publishes tail/count before finishing the entry. Ryujinx publishes
after copying; inspected Yuzu/Eden also advance metadata first. This is a
concurrency concern, but #230 already tested the order-only correction without
fixing the symptom. It is not repeated or presented as the cause.

Connected Strato controllers update their selected ring and SystemExt. Other
rings retain history until initialization/disconnection; Ryujinx publishes
disconnected entries to unused rings every input update, while the inspected
Yuzu/Eden path primarily updates the active ring and SystemExt. Frozen unused
history is therefore not uniquely a Strato defect. Strato's connection bits
are FullKey `0x3`, Handheld `0x3F`, JoyDual `0x15`, JoyLeft `0x5`, JoyRight
`0x11`; the corresponding active Eden values agree. Ryujinx uses a reduced
Handheld attribute set. Style changes can retain history; no observation yet
identifies a stale history as the game's effective input.

The SDK overload requested by the guest must be distinguished from the ring
actually read. The System/SystemExt relationship differs between old SDK
declarations and libnx's System adapter, which reads SystemExt and translates
buttons while zeroing sticks. The new diagnostic searches all seven rings
for correspondence instead of assuming the requested overload identifies one.

## Services, shared mapping and persistent consumer state

`SetSupportedNpadStyleSet` and `SetSupportedNpadIdType` update assignments under
the Npad mutex. `ActivateNpadWithRevision` currently activates Npad without
interpreting revision. Revision-dependent SDK selection is a possible inquiry,
not a demonstrated input fault. Changing it without the actual consumer would
repeat the earlier unsupported assumptions. Orientation transforms only
single JoyCons, not active FullKey/Handheld/JoyDual sticks.

The producer uses the persistent host mapping of the same shared-memory file
mapped into the guest with MAP_SHARED. Static review finds no separate HID
payload copy that could selectively preserve nonzero axes. #231's snapshots
do not establish which payload was copied by the SDK. #326 strongly argues
against live frontend analog output but cannot distinguish stale application
state, rejected samples, a different region or execution corruption.

BasicXpad has a distinct layout. No supplied observation demonstrates that
Dark Souls reads it. No BasicXpad producer or behavioral change is introduced.

## Proven ARM64 defect and generic correction

The SIMD bank starts at context offset `0xA0`; Q31 occupies `0x290..0x29F`.
`SaveCtx` stores FPSR at `0x298` and FPCR at `0x29C`, after storing Q31.
`LoadCtx` subsequently restores a corrupted Q31. This happens even with both
system registers zero. Separately, the old C++ union exposed FPSR/FPCR over
Q0's bytes, so `GetThreadContext3` read incorrect FP system state.

The correction uses existing padding at `0x98`/`0x9C` for FPSR/FPCR. Q0–Q31,
host TLS, host SP, emulated TLS, NZCV, state pointer and magic retain their
offsets. C++ static assertions enforce the ABI. `GetThreadContext3` now reads
the actual system fields. No Npad producer, controls, timing or HID metadata
is changed.

The regression assembles and executes the **production** SaveCtx/LoadCtx
instructions using Keystone/Unicorn. On master it fails with Q31's upper
64 bits changed to zero. After correction, four cases preserve Q0–Q31,
X0–X18, FPSR/FPCR, NZCV, SP and TLS, including relocated routine copies.
This proves the context defect and correction, not Dark Souls' use of Q31.

## Temporary consumer-boundary diagnostic

`HID-SDK` entry/exit hooks inspect exported, dynamically resolved Npad state
reader calls. Only exact recognized demangled signatures are decoded:
output pointer, optional int requested length, const-u32-reference ID.
Unknown ABIs are logged and skipped. The original SDK body executes natively.
Registers, return values, input samples and output buffers are not modified
by the probe. Entry/exit hook allocation is corrected to count the six actual
instructions in each LR preservation block.

The probe records before/after output, the requested reader and ID, sequence,
axes, buttons, attributes, all ring tails/counts and exact producer matches.
It reads producer state under the producer mutex. Guest buffers use fault-safe
kernel copies, including a bounded private nonblocking pipe fallback, so an
unmap/reprotect race does not turn instrumentation into an access violation.
No sleeps, artificial timeouts or page traps are installed.

Important limits:

- `installed` proves hook installation only. `returned` proves that invocation
  crossed the hook and returned; stripped symbols and direct internal calls
  can bypass it. No returned events means **no consumer coverage**, not neutral
  input. Unsupported-symbol/no-output/coverage-error events are inconclusive.
- `rawX0` is not interpreted as a returned count: return types are absent from
  mangled names. Requested slots may be unused. An unchanged slot is not proof
  of a fresh read or successful SDK validation.
- `correspondence` requires sequence and full payload equality with a completed
  producer marker. It identifies matching stored samples, not a traced CPU load;
  multiple matches are explicitly retained. A sample can also leave the current
  17-slot history before the post-call snapshot, so no match is inconclusive.
- Native instruction-by-instruction reads, SDK retry branches and later game
  transformations are not traced. The instrumentation can perturb scheduling
  and executable layout; its presence is not proof that native execution is
  correct. Logs include the first eight calls, first-output payload changes and
  every 128th call, not an exhaustive trace.

Device validation: start at Info logging, reach the involuntary walk without
touching sticks, then move/release the left stick once and export emulation.log.
Check installed + returned coverage before interpreting anything. If covered
calls return neutral output while walking continues, investigate downstream
application state/ARM64 arithmetic rather than another producer-neutral patch.
If output stays stale or diverges, use the exact reader/sequence/ID evidence to
target the consumer. If hooks are absent/bypassed, obtain that module's symbols
and reader code before designing a further probe; do not call another header
fault proof of analog consumption.

## Validation and publication boundary

Local checks: `tests/hid/run.sh`, `tests/nce/run.sh`, `git diff --check`.
They include exact ABI rejection and safe-copy tests for readable, protected
and unmapped memory, C++ context layout and hook sizing checks. Android
compilation and live game behavior are **not** claimed as locally verified.
The clean NCE correction and temporary diagnostic are separate commits on a
new Draft PR. Android CI starts on PR opening; do not monitor its run, merge
this diagnostic or modify master.
