# Dark Souls Remastered: confirmed DebugPad consumer path

## Finding

Base: master `3065a7e3a85633d6b3faffd4bcefa6c171b5cbbb`.

The supplied main executable polls a DebugPad in addition to Npad. A nonzero
DebugPad stick overrides the matching Npad stick, regardless of DebugPad's
connected attribute. The supplied SDK's singular DebugPad reader leaves its
output untouched when its ring is empty. Strato's ActivateDebugPad previously
returned success without publishing any sample, and no update path populated
that ring. This is a demonstrated producer/consumer semantic mismatch.

A controlled execution of the original main and SDK ARM64 code reproduces
false input from previous stack contents despite all Npad axes being neutral.
Publishing an actual disconnected sample removes that input. This is a causal
reproduction in isolation, **not yet confirmation that the device's observed
walking is resolved**. The game process has not been run on Android here.

DebugPad is at HID offset 0, separate from BasicXpad. No BasicXpad use is
assumed. No game binaries, proprietary instruction listings or game-specific
conditions are included in production code. #327 is not resumed.

## Evidence from the supplied main

NSO SHA-256: `a10b182dcf7bbe4712a20af54d9ae1cddd83621b88bab3ed112fb55ee1b2122f`.
SDK SHA-256: `c88f6c116562dd1db191435b34175c934a77b1ab8009fef68533aa0405628dba`.
All three decompressed segment hashes of each file were verified.
Addresses below are module-relative and apply only to those revisions.

- Initialization at main+0x81A1A0 enables style mask 7 (FullKey, Handheld,
  JoyDual), supplies nine Npad IDs and calls InitializeDebugPad at +0x81A32C.
- Polling begins at main+0x81A698. It uses GetNpadStyleSet and singular
  GetNpadState calls at +0x81A714 (FullKey), +0x81A774 (JoyDual), and
  +0x81A82C (Handheld). They write persistent object buffers at +0x408,
  +0x430 and +0x3E0 respectively. No returned sampling count is checked.
- At main+0x81AB80 it unconditionally calls GetDebugPadState with a stack
  buffer at frame SP+0x20. That buffer is not cleared before the call.
- main+0x81ABE4..0x81AC68 selects each DebugPad stick whenever either of
  its two axes is nonzero; otherwise it uses the corresponding Npad stick.
  The DebugPad connected attribute is not checked here.
- main+0x81AC6C..0x81ACB0 stores the normalized input channels at object
  offsets +0x468..+0x474. This is the input polling output, not a traced
  character world-space movement vector.
- No imported System/SystemExt or BasicXpad reader was found in this main's
  dynamic symbol table. This is not an exhaustive proof against every possible
  indirect or separately loaded consumer.

## Evidence from the supplied SDK

- Public GetDebugPadState: SDK+0x228DF4; detail reader: +0x2160FC.
- It reads the DebugPad count at ring+0x18 and tail at ring+0x10.
- When count is zero, it returns success without writing the output buffer.
- With a sample present, it copies the 0x20-byte SDK payload after the
  8-byte outer marker, verifies that marker did not change, and returns.
- The payload is: sampling number (u64), attributes (u32), buttons (u32),
  right stick X/Y (i32 each), left stick X/Y (i32 each). The shared-memory
  storage stride is 0x28. A disconnected state still writes four zero axes.

The previous 108-case Npad SDK audit remains useful for Npad validation, but
it could not expose this additional consumer without the main executable.
Neither neutral producer logs nor #231 header/SixAxis traps proved analog
consumption. The new replay executes the actual analog-copy and selection code.

## Reference semantics

- [Eden e0d47f2](https://github.com/eden-emulator/mirror/blob/e0d47f2790e3f758fa2188926a0bb2f56af6b310/src/hid_core/resources/debug_pad/debug_pad.cpp): when activated, publishes sampling
  increments even with debug_pad_enabled false; default payload is disconnected.
- [Yuzu 7ffac53](https://github.com/yuzu-emu-mirror/yuzu-mainline/blob/7ffac53c9e67fa18d067114a8eb3e77c74bba68f/src/hid_core/resources/debug_pad/debug_pad.cpp): same active-but-disabled sampling behavior.
- [Ryujinx 534f925](https://github.com/kotx/Ryujinx/blob/534f92506bf8c6aba29bb86917bf10954a360d8e/src/Ryujinx.HLE/HOS/Services/Hid/HidDevices/DebugPadDevice.cs): publishes a new zero state with incrementing sampling number on update.

Implementation is independently written against the ABI and behavior. It does
not port GPL reference implementation code.

## Generic correction

Define the existing DebugPad storage fields explicitly without changing section
size or offsets. ActivateDebugPad validates the resource ARUID, then publishes
one disconnected neutral sample immediately. The existing input update callback
advances its sampling number thereafter. Activation is idempotent. A manager
mutex serializes activation and updates. Marker/fence/payload/marker publication
precedes release publication of ring metadata.

There are no host bindings for DebugPad. Its connected attribute remains zero.
Npad publication, analog values, frontend mappings, game timing and the Q31
correction are outside this change. No logging, page traps, sleeps, artificial
timeouts or Title-ID conditions are added. Existing HID scheduling is reused.

## Verification

`bash tests/hid/run.sh` compiles the production manager and storage layout and
checks inactive state, immediate activation, idempotence, wraparound, sequence
continuity, disconnected payload, and serialized concurrent writers. Existing
Npad assignment tests also run. It does not model concurrent guest loads.

Optional private replay (Python packages lz4 and unicorn, plus a C++20 compiler):

```sh
python tests/hid/dsr_debugpad_replay.py --main /private/main --sdk /private/sdk
```

The script rejects different NSO hashes and never downloads binaries. It
relocates original main/SDK code, prepares an already initialized SDK mapping,
and executes the input polling function and SDK readers, including the SDK's
original memcpy. It obtains corrected bytes by compiling/running the actual
production manager test. It does not reimplement the game's input selection.

18 executions cover FullKey/JoyDual/Handheld, neutral and active Npad axes, and
three conditions: empty DebugPad with previous stack data, production DebugPad
sample with the same previous data, and empty DebugPad with clean stack.
Production output must exactly match the clean-stack baseline; nonzero Npad
input must remain intact. With neutral Npad, stack values (16384,-8192) yielded
input channels approximately (0.6000092,-0.3000046) before correction and zero
after the production disconnected sample. Other chosen old stack values can
produce different directions; no actual device stack values are inferred.

Limits: initialized mappings and static snapshots are supplied by the test.
IPC startup, Android memory ordering under live guest reads, NCE execution and
on-device gameplay remain unverified. The private replay is not part of CI and
requires user-supplied binaries; standard host tests need no proprietary data.

This reproducible mismatch justifies the next Android build. The existing
pull_request workflow starts it when the Draft PR is opened; Actions will not
be monitored. Device testing should check idle, move/release and both handheld
and docked controller modes before treating the walking symptom as fixed.
