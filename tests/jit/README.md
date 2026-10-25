# jit:u runtime validation

This suite compiles the production NRO loader, plugin CPU, CodeMemory/VMM and
IPC implementations. Platform scaffolding replaces Android logging, handle
transport and ASharedMemory_create; the latter uses Linux memfd with real shared
pages. OpenSSL independently provides SHA-256 for the IPC authentication test.
The test NRO's GenerateCode callback emits an actual ARM64 `mov w0, #42; ret`.
A second ARM64 CPU executes the emitted bytes from the shared CodeMemory pages.
`plugin_fixture.S` records the source of its instruction encodings.

Use Clang, Boost headers, OpenSSL development headers, and the repository's
Dynarmic submodule. Build Dynarmic's A64 frontend for the host:

```sh
cmake -S app/libraries/dynarmic -B /tmp/strato-jit-dynarmic \
  -DDYNARMIC_FRONTENDS=A64 -DDYNARMIC_TESTS=OFF -DDYNARMIC_INSTALL=OFF \
  -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF
cmake --build /tmp/strato-jit-dynarmic -j4
DYNARMIC_BUILD_DIR=/tmp/strato-jit-dynarmic tests/jit/run.sh
```

Without DYNARMIC_BUILD_DIR, the runner checks the loader and CodeMemory only.
JIT_SANITIZER_FLAGS applies to the portable loader tests. The compact VMM
fixture avoids reserving Android's full 140 GiB address space on Linux.

Coverage includes MOD0 symbols, ELF RELA/RELR and imports, BSS, malformed
metadata, terminal RELR bitmaps, actual shared RX/RW aliases, duplicate/partial
mapping rejection, source restoration, stack arguments, exclusive instructions,
cache invalidation, cancellation on process exit, helper failures, NRR digest
rejection, optional empty IPC buffers and full GenerateCode output serialization.

## Reference comparison

- Eden `10bcd2d849843b146a79c61a21df615e1bdcfcde`:
  `src/core/hle/service/jit/{jit,jit_context,jit_code_memory}.cpp`.
- Yuzu mirror `d9e3e3775b125ac1856273b450141d6a87e6e470`: same files.
- Ryujinx mirror `043ea22b6668cc8678efffc011732ea7467e6ca5`:
  `src/Ryujinx.HLE/HOS/Kernel/Memory/KCodeMemory.cs`. This inspected tree does
  not implement the jit:u plugin service; comparison is limited to the kernel
  CodeMemory ownership/mapping rules.

Eden/Yuzu establish the five-range configuration and callback ABI, the private
ARM64 sysmodule CPU and identity mappings into user code/transfer memory.
The Strato implementation uses its existing Dynarmic version, maintains R/RX
permissions on owner mappings and a distinct writable physical view, handles
bounded ELF symbol/relocation tables, rejects unsupported imports/instructions,
and preserves cache-invalidation requests across CPU handoff. Scheduling slices
allow process-exit cancellation and do not impose a compilation timeout.

The #324 NRR/NRO checks and bounded diagnostic symbol scanner remain in place.
This suite validates the implementation with synthetic inputs, not the retail
Super Mario 64 plugin, Android's ARM64 backend, or gameplay. Android builds and
on-device gameplay remain separate validation steps. No game IDs or fake
successful compilation paths are used.
