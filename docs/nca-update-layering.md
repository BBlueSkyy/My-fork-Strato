# Program NCA update layering

Status: **IMPLEMENTADO — AGUARDANDO VALIDAÇÃO EM JOGO**.

Base: `075d1baf8a5d84a291c9bb9d0123c347a6832e46` (`master`).
Branch: `fix/nca-update-layering-mk8`.

## Findings and limits of the diagnosis

The old implementation exposed the decrypted physical section of an unresolved
BKTR patch as `romFs`. NSP/XCI selection required both ExeFS and RomFS to be
present, and assigned the last matching Program NCA encountered in the container.
ExeFS was replaced while loading the process; FSSRV later reconstructed the patch
NCA for each storage opening. The base supplied to BKTR was a cropped IVFC data
level, with a manually subtracted offset, rather than the matching whole section.

The old reader always selected IVFC array element 5 and counted present sections
instead of preserving all four FS indices. It read the indirect table through
ordinary AES-CTR before applying the subsection generations. This is incorrect
when that metadata uses a different AES-CTR-Ex generation. The encrypted fixture
deliberately exercises this case.

Two additional BKTR defects were reproduced against the original implementation:

- An unencrypted partition read used the full output span instead of the requested
  partition length, returning too many bytes and potentially overwriting the next
  relocation's output.
- Unaligned encrypted reads masked a 64-bit physical address with a 32-bit mask,
  losing the high bits above 4 GiB.

These are demonstrated implementation defects. They do **not** establish which
defect caused Mario Kart's HOS-22 crash. No game files, console keys, functional
APK, or new device run were available for byte comparison in this session.

## Resolution and storage ownership

`OS::Execute` resolves content once before `LoadProcessData`. External updates and
DLC use the NSP format accepted by the content picker, independently of the base
container format. The user-selected external update takes precedence over an
embedded patch.

CNMT Program records select the initial Program (`idOffset == 0`), preserve base
and patch candidates separately, and associate patch versions with the base
Program ID. Conflicting records are rejected. Containers without Program records
have an unambiguous-header fallback; no game-specific IDs are used.

The loader owns `processExeFs`, `currentProcessRomFs` and `patchDataRomFs`.
`main.npdm`, `rtld`, `main`, `sdk` and the other NSOs use `processExeFs`. A patch PFS
must contain both `main` and `main.npdm`; a valid partition without those files
does not replace the base ExeFS. Malformed partition metadata is rejected.
User LayeredExeFS/LayeredRomFS mods are applied once after NCA resolution.

The NCA path is:

1. Preserve the actual section index and validate its extent.
2. Apply the existing sparse mapping, if present.
3. Decrypt normal sections with their existing AES-CTR backing. For AES-CTR-Ex,
   read its table using the header counter, then create bounded encrypted/clear
   extents using each entry's generation and the absolute physical counter.
4. Read the indirect table **through** that decrypted view. Compose it with the
   whole corresponding decrypted section of the base. Check every relocation's
   source, length and boundary before exposing the result.
5. Select IVFC data with `levelCount - 2` (the count includes the master hash),
   validating levels against the resulting virtual storage.
6. Apply the existing compression builder to the IVFC data region, then validate
   the final RomFS header and metadata bounds.

Byte-swapped/inconsistent IVFC counts and out-of-range data are rejected; there is
no heuristic search for a convenient header and no ignored corruption warning.
This change does not add cryptographic verification of all IVFC hash blocks.
Pre-existing limits on multilevel bucket trees and unsupported encryption/hash
variants remain; this is not a port of the complete Horizon filesystem driver.

`OpenDataStorageByCurrentProcess` serves the persistent current-process backing.
It does not reconstruct NCA/BKTR or rescan mods. Command 203
(`OpenPatchDataStorageByCurrentProcess`) serves the same mounted view when a
Program patch actually supplies RomFS data. Without such data, including a
base-only title or an ExeFS-only update, it returns `EntityNotFound`. An
unconditional fallback to the base was deliberately not adopted: the public HOS
interface identifies a separate patch-data operation, and does not establish
that a base-only storage is a valid substitute. Device validation of command 203
is still required.

DLC/PublicData storage uses its own NCA RomFS. It is no longer reconstructed
against the running Program's raw RomFS. System-archive and asset fallbacks are
unchanged.

`sparse_storage.*`, `compressed_storage.*`, and `region_backing.*` are unchanged.
`bktr.*` retains its implementation, with the two reproduced read fixes and a
constructor for already-decrypted whole-section inputs.

## Validation

Run `bash tests/vfs/run.sh /tmp/strato-vfs-tests` after initializing the fmt,
mbedtls and lz4 submodules. A CMake target is also provided in `tests/vfs`.
The harness compiles the production NCA, BKTR, SparseStorage, CompressedStorage,
CTR, AES, PFS, CNMT, Program selection and content-resolution code. Host shims
replace Android platform types/logging; mod discovery is an identity operation
because these fixtures install no mods. AES uses real mbedTLS and compression
uses real LZ4, with synthetic keys and data.

| Required scenario | Automated coverage | Device coverage |
| --- | --- | --- |
| Base without update | ExeFS, RomFS, variable IVFC levels, FS-index holes, absent patch data | Pending |
| Normal update | Complete ExeFS replacement, coherent NPDM/NSOs, replacement RomFS | Pending |
| NSP/Program BKTR | CNMT selection and order independence; encrypted/clear NCA layers; cross-boundary reads | Pending |
| Sparse NCA | Physical remapping and zero range with unchanged storage implementation | Pending |
| Compressed NCA | LZ4 output and indirect composition before decompression | Pending |
| DLC/DataId | Independent PublicData NCA backing; FSSRV uses that existing backing | IPC/device run pending |
| Persistence | Backing survives destruction of NCA/update objects; no repeated resolution | Pending |
| Invalid metadata | Reject IVFC count, PFS extents, RomFS bounds, invalid physical relocations and mismatched Program IDs | Not applicable |

14 test groups passed locally. `git diff --check` passed. The local Android
build attempt failed before compilation because the Gradle distribution download
was unavailable on this host. The existing PR workflow is the Android build gate;
its final result and APK links are recorded in the delivery message/PR.

## Device comparison

Use identical base/update content and identical settings for both builds.
Start without mods so the fingerprint describes the NCA layers being compared.
The loader logs the final size, the first 16 bytes, and FNV-1a/64 fingerprints of
three deterministic blocks (up to 4096 bytes each, with offsets/lengths included).
FSSRV logs that same identity when serving the storage. These are diagnostic
fingerprints, not cryptographic integrity checks or a complete byte comparison.

For the supplied Mario Kart reproducer, compare the log to the user's reference:

- `Program patch ExeFS selected`.
- `BKTR Program patch RomFS constructed`.
- `OpenDataStorageByCurrentProcess`: size `0x2D1288168`, first 16 bytes
  `5000000000000000F0C721D102000000`.
- HOS-22 survives the `kind=219` mapping at `0x57AB50000` and reaches the subsequent
  remaps: size `0x20000`, then offset `0x20000`/size `0x40000`.

Those values appear only here as validation references, never in implementation
selection or runtime behavior. Success still requires a new device log and tests
of the base/update, sparse/compressed and DLC games that previously worked.
Scheduler/kernel PR #146, NCE, GPU/texman, NvMap/GMMU/MapBufferEx, audio, HID,
timeouts and sleeps are outside this diff.

## Format references

- [Atmosphère NCA filesystem driver, pinned source](https://github.com/Atmosphere-NX/Atmosphere/blob/cb4b882e3b176480ac57a1161a85ff175c3f162c/libraries/libstratosphere/source/fssystem/fssystem_nca_file_system_driver.cpp)
  — whole-section originals, AES-CTR-Ex metadata, indirect storage and layer order.
- [Atmosphère NCA header](https://github.com/Atmosphere-NX/Atmosphere/blob/cb4b882e3b176480ac57a1161a85ff175c3f162c/libraries/libstratosphere/include/stratosphere/fssystem/fssystem_nca_header.hpp)
  — FS/hash fields and IVFC level indexing.
- [Switchbrew filesystem interface](https://switchbrew.org/wiki/Filesystem_services#fsp-srv)
  — commands 200, 202 and 203. It does not document all command-203 fallback semantics.
