# Synthetic XCI/container regression tests

These tests compile the real XCI and NSP loaders, NCA parser, CNMT selector, common
content resolver, HFS0/PFS0 parser, AES and storage layers. Every container,
content archive and encryption key is generated synthetic data. Android
process loading and user mods are outside the fixture scope; their host shims
throw on process-loading calls or return the supplied filesystem unchanged.

Run with the populated fmt, mbedTLS and LZ4 submodules:

```sh
bash tests/xci/run.sh
```

Or use CMake (CMake 4 requires the policy option for the older mbedTLS project):

```sh
cmake -S tests/xci -B /tmp/strato-xci -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/strato-xci --target xci_tests vfs_tests -j4
ctest --test-dir /tmp/strato-xci --output-on-failure
```

The executable accepts a substring filter, for example `xci_tests 'key area
generation 0x21'`. `_GLIBCXX_ASSERTIONS` is enabled on the XCI target, including
the real NCA implementation, to make unsafe array accesses fail reproducibly.

## Reproduction and verification

Before the production fixes, running the executable against master `9935f133`
with these fixtures gave:

* `XCI`: selected Program hidden from the common Loader resolver; an unselected
  Program with malformed inner PFS0 failed during container construction;
  an impossible root header hash span was accepted (exit 1).
* `PFS0` and `HFS0`: file data offset `UINT64_MAX` was accepted (exit 1).
* Each of `key area generation 0x21`, `key area generation 0xff`, `title kek
  generation 0x21`, and `title kek generation 0xff`: libstdc++ array bounds
  assertion in the real NCA key lookup (exit 134).

After the fixes all ten groups pass. The key-area cases cover Application,
Ocean and System arrays. The deferred Program fixture selects each ProgramIndex
in turn in both real container loaders: index 0 resolves executable content and
RomFS despite index 1's bad inner PFS0; selecting index 1 still fails when the
common resolver opens it. An additional pair of valid Programs verifies that
XCI and NSP select identical Program IDs, distinct ExeFS markers and distinct
RomFS bytes for both ProgramIndex 0 and 1.
Structural cases also reject invalid names, missing terminators, impossible
entry counts, oversized files, short reads and oversized HFS0 hashed prefixes.
The existing VFS suite also passes in the same CTest run.

## Reference comparison

References were inspected on 2026-10-29; their roles are deliberately separate:

* [Eden card_image.cpp, commit a2aa4d97e10a8345babfe647e1abd5ed5909c623](https://github.com/eden-emulator/mirror/blob/a2aa4d97e10a8345babfe647e1abd5ed5909c623/src/core/file_sys/card_image.cpp#L43-L44)
  constructs the root HFS view from `hfs_offset` through the remaining file size.
  This directly supports the root partition span used here.
* [hactool xci.c, commit 1d64a83450e025622f3468c28fc4164dad2c5ef6](https://github.com/SciresM/hactool/blob/1d64a83450e025622f3468c28fc4164dad2c5ef6/xci.c#L76-L83)
  passes `hfs0_header_size` directly to its hash checker without multiplying by
  four. It is a byte count for the root header's hashed span, not the root
  partition's total content size.
* [hactool hfs0.h at the same commit](https://github.com/SciresM/hactool/blob/1d64a83450e025622f3468c28fc4164dad2c5ef6/hfs0.h#L19-L26)
  defines the HFS entry's hashed prefix size and 64-bit reserved field; the
  reserved field precedes the 32-byte hash at offset 0x20.

The original public `Thealexbarney/LibHac` and `Ryujinx/Ryujinx` GitHub
repositories returned 404; `ryujinx-mirror/ryujinx` returned HTTP 451. These
unavailable repositories are not claimed as verified sources for this fix.
The separate launch investigation compares the available Ryubing source.

## Limits

The former XCI header-size mistake was partly masked by `RegionBacking`
forwarding unchecked reads to its parent: declared-size correctness cannot be
inferred merely from an existing title reaching RomFS resolution. Bounds checks
and the root-span fix are needed together. Program header and section-descriptor
validation still runs during discovery; only opening Program content is
deferred, as in the NSP loader. Meta and Control discovery remains eager.

These are container/storage regressions, not proof that any specific title's
native crash is fixed. The supplied Nicktoons log already reaches resolved
RomFS, so the investigation must continue past that stage using startup
diagnostics. This change does not implement new gamecard security modes or
cryptographic HFS0 hash verification.
