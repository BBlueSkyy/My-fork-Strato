# IPC and native SVC diagnostics tests

Run `bash tests/ipc/run.sh` on a Linux host with GCC and C++20.

The transport test compiles the production IPC parser and header, substituting
only process/session/logging scaffolding. It checks HIPC argument size excluding
CMIF overhead, TIPC/domain layouts and the shared input/output view of W buffers.
The native test compiles the production trampoline emitter, checks register
selection, slot count and unchanged assembly context offsets, then exercises
permission-callback based frame traversal and corrupt frame rejection.

This does not execute the native NCE stack/TLS switch on an Android ARM64 device.
See `docs/jit/sm64-crash-provenance.md` for the retail crash evidence and limits.
