# NPDM host regression checks

Run `bash tests/npdm/run.sh` (or set `CXX` to a compatible compiler).
This compiles the production `vfs/npdm.cpp` and `vfs/backing.h` with small
host declarations for scheduler types, memory enums, logging and common utilities.
The parser is not reimplemented. AddressSanitizer and UndefinedBehaviorSanitizer
are enabled. Leak detection is disabled because the sandbox's process tracing
prevents LeakSanitizer from inspecting `/proc`.

Synthetic valid META/ACI0/KAC data verifies the existing priority bit ordering,
core affinity, kernel version and flag decoding. Malformed fixtures cover parent
extents, header overlap, short reads, duplicate/missing ThreadInfo, reversed
ranges and invalid main-thread affinity/priority.

Before the parser fix the checks demonstrated that it accepted an ACI0 extent
past EOF, KAC outside/overlapping its ACI0, duplicate ThreadInfo, out-of-parent
filesystem/service controls, and short META/ACI0/KAC reads. Invalid core indices
escaped as `std::out_of_range` from `std::bitset` instead of an NPDM parse error.
The missing-ThreadInfo check already rejected the fixture, but the parser read
uninitialized priority bounds first; the fix makes this rejection deterministic.
Two default-constructor designated initializer lists are ordered to compile with
GCC; their values are unchanged. GCC still emits its existing packed bit-field
layout warnings; size assertions and the decoding fixture pass.

Primary semantic references inspected on 2026-10-29, Atmosphere revision
`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`:

- https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/stratosphere/loader/source/ldr_meta.cpp
  (`ValidateSubregion`, `ValidateNpdm`, `ValidateAci`): ACI0 must follow META;
  all its child extents must follow the ACI0 header and fit inside ACI0.
- https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/kern_k_capabilities.cpp
  (`SetCorePriorityCapability`): core/priority capability may only be set once;
  ranges must be ordered and core IDs must be supported.
- https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/stratosphere/loader/source/ldr_capabilities.cpp
  (`KernelFlags`): confirms field positions and ordering.

These checks do not establish a cause for a game launch stopping before NPDM
parsing. They do not emulate execution, authenticate ACID/signatures, validate
all ignored capability kinds, or change flag meanings. The existing four-core
scheduler limit and priority interpretation are preserved. KAC sizes continue
to be divided by the descriptor size, as in the inspected loader reference;
there is no new speculative alignment restriction.
