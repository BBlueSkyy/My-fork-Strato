# Pokémon Legends Z-A: NGC audit

The original PR registered only `GetContentVersion`. Unknown commands followed
the global service fallback and could return Success without a result payload.
This revision registers all four application commands and connects them to the
original firmware dictionary format. **This is a bounded implementation, not a
complete port of the reference profanity filter.** Unsupported cases return an
explicit error; they never receive a fabricated clean mask or masked-word count.

## Reference and wire contract

Audited reference: [kotx/Ryujinx, commit
534f92506bf8c6aba29bb86917bf10954a360d8e](https://github.com/kotx/Ryujinx/tree/534f92506bf8c6aba29bb86917bf10954a360d8e/src/Ryujinx.Horizon/Sdk/Ngc).
The complete `Detail/ProfanityFilter.cs`, `ProfanityFilterBase.cs`, dictionary
reader/serialization helpers, option enums and IPC `Service.cs` informed the
supported behavior. The dictionary backend retains the reference MIT license.

| Command | Raw input | Buffers | Success payload |
| --- | --- | --- | --- |
| 0 GetContentVersion | none | none | `u32` version |
| 1 Check | `u32 regionMask`, 0x14-byte option | input map-alias text | `u32 checkMask` |
| 2 Mask | same as Check | input map-alias text, output map-alias text | `i32 maskedWordsCount` |
| 3 Reload | none | none | none |

The option contains five consecutive `u32` fields: `skipAtSignCheck`,
`maskMode`, `flags`, `systemRegionMask`, `reserved`. IPC handlers read the full
24-byte raw argument prefix with `memcpy` and
reject truncated arguments or absent buffer descriptors. They accept a present
zero-length buffer. These malformed wire cases use the repository's
`kernel::result::InvalidArgument` policy (`{1,14}`); this is an emulator boundary
policy, not a claim that reference NGC itself generates that result.

The facade also enforces map-alias transport: Check requires exactly one A
descriptor and Mask exactly one A plus one B. X pointer, W exchange and C
receive-list descriptors are rejected. Because the generic parser merges
descriptor families into common buffer vectors, this check prevents accidental
acceptance of an X buffer as NGC's A input or a W/C buffer as its B output.
Rejecting extra descriptors/counts is an explicit fail-closed emulator policy;
this is not a claim about native treatment of every superfluous descriptor.

The generic request parser's non-domain `cmdArgSz` includes CMIF headers and
padding that precede `cmdArg`, so that field alone cannot bound a service read.
NGC independently derives its raw frame from header/handle/PID/buffer descriptor
counts, 16-byte total CMIF padding and the 0x100-byte TLS extent. It validates
the expected payload/argument pointers, caps domain payload and input-object
sizes, and rejects domain size underflow. This prevents truncated raw frames
from consuming stale TLS argument bytes without changing the generic parser.

Check ends its input at the first NUL and validates strict UTF-8 before accessing
dictionaries. Malformed UTF-8 returns NGC `InvalidUtf8Encoding` (`{146,5}`). Mask
first copies `min(input.size, output.size)` bytes, as `Service.cs` does, then
processes the full output descriptor. A filter failure adds no scalar payload.
Filter changes are committed only on success, so on error the output contains
the initial IPC copy. The reference can mutate output before a later dictionary
read fails; this implementation deliberately provides a narrower failure-side
effect contract.

## Firmware data

The provider reads system data title `0100000000000823` from the existing
`switch/nand/system/Contents/registered` convention. It requires matching title ID,
NCA Data content type and a readable RomFS. Flat `.nca` files and `.nca/00`
layouts are accepted. Multiple matching archives are rejected rather than
selecting a firmware version by directory order without CNMT selection.

An explicitly supplied extracted RomFS may instead live at
`switch/ngc/0100000000000823`, under the app's public files directory; this takes
precedence. Files must retain their original names and bytes. Nothing creates
this directory automatically or bundles Nintendo word lists. All files used by
one provider belong to one mounted source; files from different archives are
never combined.

`version.dat` is interpreted as a four-byte big-endian prefix. Missing, unreadable
or shorter data yields version 0, matching the reference's fallback. A longer
file uses its first four bytes: the reference's size condition uses `&&`, then
reads four bytes. Version 0 says nothing about the ability to check a word.

The required dictionaries are gzip archives named
`ac_common_{not_b,b1,b2}_nx`, followed by all three
`ac_<region 0..15>_{not_b,b1,b2}_nx` archives for each selected region. Both
operations always read the common dictionaries, even when the requested region
mask is zero. Region selection uses the low 16 bits of
`regionMask | option.systemRegionMask`; common matches report that union where
the reference does. Missing or malformed required dictionaries return NGC
`DataAccessError` (`{146,102}`), including when only a selected regional archive
is absent. Compressed and decompressed archive sizes are bounded to 64 MiB.
The reference explicitly translates partial/complete dictionary failures to that
NGC result but can propagate an underlying filesystem error for the third,
delimited dictionary. This implementation normalizes all three to
DataAccessError because its reader reports availability rather than preserving
filesystem-specific failure codes.

Reload discards the mounted source and retries discovery. An unmounted source
is a supported uninitialized state, as in `ContentsReader.Reload`; Reload can
succeed while a subsequent Check still returns DataAccessError. Dictionary
parsing happens on each filter operation, so there is no stale dictionary cache.
Failure to remount a previously mounted source returns DataAccessError. Provider
discovery/index limits are 4096 entries, 64 levels, 4096-byte paths and an 8 MiB
RomFS index; individual files are limited to 64 MiB.

## Implemented behavior and remaining limits

Available dictionaries are actually decompressed and decoded. The backend reads
the serialized compressed integer arrays, sparse sets, Aho-Corasick transition,
failure and output links, and balanced-parenthesis tree. It derives rank/select
operations from the underlying vectors and bounds-checks/skips the serialized
acceleration tables. It does not substitute a hand-written word list.

For ASCII input, Check implements invariant lowercase, partial and delimited
dictionary matches, common/region masks and the reference's at-sign behavior.
Reference Check ignores the skip mode, mask mode and canonicalization flags;
only its system-region option affects matching. Mask implements flags=0,
both mask modes, the email state machine when enabled, partial matches,
separator removal, whole-word delimiter checks, and the 512-character processing
limit. The tail beyond that limit and bytes after a natural NUL are preserved
according to the supported ASCII path.

The following cases return SF `NotSupported` (`{10,1}`), an explicit emulator
limitation rather than a guessed hardware NGC result:

- Valid non-ASCII Check input, because invariant Unicode lowercase is not ported.
- Any non-ASCII Mask input within the processed prefix. Default reference Mask
  uses permissive conversion, so malformed Mask bytes are not misreported as
  strict Check-style UTF-8 errors.
- Mask normalization/similar-form flags, undefined mask/skip modes, matched spans
  of 255 bytes or longer, and whole-word candidates that exceed the reference's
  64-byte scratch buffer.

These explicit unsupported-input checks can run before a dictionary lookup;
they are emulator capability errors, not reference data-access error ordering.

Unicode lowercase, FormKC normalization, half-width Kana handling, similar-form
archives/tables, Unicode separator categorization and the reference's long-mask
marker encoding remain unimplemented. Full Pokémon name-entry compatibility
cannot be claimed from these tests, especially for Japanese/non-ASCII names or
titles requesting canonicalized masking. No on-device game run was performed.

## Verification

`bash tests/ngc/run.sh` compiles the production IPC handlers and dictionary
backend with AddressSanitizer and UndefinedBehaviorSanitizer. It calls handlers
through a small host dispatcher that consumes the production `SERVICE_DECL`
registration. The kernel HIPC packet decoder and global BaseService are outside
this host suite; the provider is replaced by a callback because provider loading
has its own suite.

The IPC suite covers all command IDs, every truncated raw argument length,
inflated generic raw sizes, shifted handle/PID and buffer descriptor layouts,
TLS overflow, pointers outside the frame, domain payload underflow/overrun and
input-object padding overlap, correct A/B layouts, wrong/mixed X/W/C transport,
extra A descriptors, missing/empty descriptors, malformed UTF-8 and NUL ordering,
missing dictionaries, unavailable selected regions, shorter/larger Mask buffers,
valid real-format fixture matches, exact result/count widths, both mask modes,
unsupported flags, the 512-character boundary, big-endian version values,
missing/short/long version data and Reload. The prior command-1 behavior was
observed failing the missing-data result assertion before implementation.

`bash tests/ngc_dictionary/run.sh` separately exercises production parsing and
matching using generated synthetic tokens serialized in the real archive
format, including compressed integer blocks and sparse width-zero encoding.
These are format fixtures, not Nintendo firmware data. The backend parser was
also checked against the genuine reference embedded separator trie without
adding it to the repository.

LeakSanitizer is disabled for the IPC harness because the execution environment
blocks its `/proc` thread inspection; address/undefined instrumentation remains
enabled. See `tests/ngc_dictionary/README.md` for backend details and the
independent `tests/ngc_content` suite for provider validation.
