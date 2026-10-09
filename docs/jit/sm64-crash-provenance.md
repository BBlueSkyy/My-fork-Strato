# SM64 abort after jit:u preparation

## Evidence and limits

The supplied `emulation.log` reports the SM64 process of Super Mario 3D All-Stars
1.0.0 (program index 1) and the following sequence:

- `LoadPlugin` completes at 5,976,619: version 1, RX
  `0x19A4300000 + 0x18000000`, RO `0x19BC300000 + 0x200000`.
- `SetTerminateResult` receives 3,072,514 at 8,161,400.
- `svcBreak` follows at 8,161,428 with reason 0, address `0x19A4273278`, size 4.

3,072,514 is `0x002EE202`: module 2, description 6001. The fork names this
result `fssrv::result::InvalidArgument`. That identifies the result encoding,
not the call which produced it. `SetTerminateResult` is a guest notification:
it reads the application's supplied result and returns success. It does not
produce 2-6001. Reason 0 is the Horizon userspace Panic break reason.

The old log does not contain the break payload, native caller PC, native guest
stack or failed IPC command. It also does not expose nonzero plugin callback
outputs at the normal log level. Therefore it cannot establish whether the
result originated in an HLE service, SDK validation, a plugin callback, or the
application. There is no evidence here establishing a filesystem content bug.
Successful preparation proves the preparation callbacks returned, not that
Control/GenerateCode subsequently succeeded.

## Reference comparison

Source snapshots inspected:

- [Eden](https://github.com/eden-emulator/mirror/tree/10bcd2d849843b146a79c61a21df615e1bdcfcde) source snapshot
  `10bcd2d849843b146a79c61a21df615e1bdcfcde`: `jit/jit.cpp`,
  `jit/jit_context.cpp`, `service/hle_ipc.cpp`, `kernel/svc/svc_exception.cpp`.
- [Yuzu](https://github.com/yuzulator/yuzu-mainline/tree/d9e3e3775b125ac1856273b450141d6a87e6e470) source snapshot `d9e3e3775b125ac1856273b450141d6a87e6e470`:
  `jit/jit.cpp`, `service/hle_ipc.cpp`, `kernel/svc/svc_exception.cpp`.
- [Ryujinx](https://github.com/Bl1ndBeholder/Ryujinx/tree/043ea22b6668cc8678efffc011732ea7467e6ca5) source snapshot `043ea22b6668cc8678efffc011732ea7467e6ca5`:
  `HOS/Ipc/IpcMessage.cs`, `HOS/Kernel/Memory/KCodeMemory.cs`,
  `HOS/Kernel/SupervisorCall/Syscall.cs` and `IApplicationFunctions.cs`.
  This snapshot has no jit:u implementation; it cannot serve as evidence for
  plugin execution behavior.

Eden/Yuzu distinguish plugin outputs from the IPC result: GenerateCode writes
its result through a pointer; Control has both a wrapper return and an output
result. They warn on unsuccessful callbacks. Their break handling reads a
four-byte diagnostic buffer for ARM64 and emits a guest backtrace. Ryujinx
also records the termination result supplied by the guest and prints the guest
stack/registers on fatal breaks. None of these observations identifies the
producer of the supplied log's 2-6001.

## Changes

Two transport errors were reproduced with production IPC parsing in host tests:
non-domain HIPC `cmdArgSz` included the CMIF header and fixed 16 bytes of padding;
W exchange buffers were exposed twice as output and never as input. The parser
now excludes that overhead and exposes one shared input/output view for W.
These are contract corrections, not a demonstrated explanation of the retail
crash. TIPC and domain argument layouts remain covered by regression tests.

Failed IPC calls now report their service, function, command, raw result and
native caller. Control logs its command and both returns; GenerateCode logs
nonzero callback results and the first generation at normal log level.
Preparation and GetCodeAddress report their actual values. Successful generated
blocks continue using the existing shared CodeMemory and I-cache synchronization.

The native trampoline passes the original SVC PC directly; it does not attempt
to read the hidden Reserved `.patch` mapping through guest permissions. It also
captures guest SP/FP and reads the original LR on a best-effort basis. All prior
ThreadContext assembly offsets are unchanged. Break payload and frame reads
validate read permissions while holding the VMM mapping lock. Diagnostic signal
faults are handled locally; frame traversal stops on invalid frames/cycles and
limits output without limiting execution. Fatal/notification-only break behavior
is preserved.

## Verification

`tests/ipc/run.sh` exercises production HIPC/TIPC/domain parsing, exchange buffers,
actual native trampoline emission, context layout and bounded frame traversal.
`tests/jit/run.sh` with the A64 Dynarmic host build covers existing relocation,
CodeMemory, ARM64 execution, generation and cache tests, plus failed callbacks
carrying `0x002EE202` without confusing that output with the IPC failure.

The retail crash has not been reproduced locally. A new on-device run is needed
to establish its exact producing call from these diagnostics. Do not describe
these changes as a confirmed SM64 crash fix until that evidence exists.
