# DLC failure isolation — Dragon Ball Xenoverse 2

## Context

The reported title `010078D000F8900A` is listed as add-on content by its
CNMT, but its data NCA fails RomFS validation (`Invalid resolved RomFS header`).
An independent integrity check in Eden flags the same data NCA, and its SHA-256
does not match the CNMT content hash. An incorrect hash alone does not prove which
RomFS layer is wrong.

## Recovery rules

- **Only optional DLC NSPs** are isolated. Main-game and update errors remain
  fatal, so a broken executable or patch is not disguised as a success.
- Handle each selected DLC individually; **publish a loader only after its
  entire NSP has parsed** and the CNMT is `AddOnContent`.
- For a DLC whose CNMT lists a Data content record, require a data NCA with
  usable RomFS backing. Metadata-only DLCs remain supported.
- If the NSP raises a loader/standard parsing exception, log the selected DLC
  index and detailed reason, reject it and attempt the next DLC. Do not absorb
  `std::bad_alloc` (global memory exhaustion).
- `aoc:u` advertises only accepted DLCs. List requests clamp the starting
  offset, requested count and output buffer size to avoid out-of-bounds access.
- Never treat invalid metadata or a missing data storage as valid content,
  never patch/overwrite the NSP, and do not disable RomFS validation.

This is *startup error isolation*, not full cryptographic integrity
verification: a structurally readable NCA whose data silently differ from
the CNMT hash may still be accepted. Streaming full SHA-256 validation for
every DLC is intentionally out of scope to avoid expensive startup reads.
Invalid content that causes a native signal instead of a C++ exception also
cannot be safely recovered with these exception handlers.

## Logs expected with 82 valid / 1 invalid DLC

```
Rejected DLC X/83: loader result 1: ... Invalid resolved RomFS header
Skipped 1 invalid/unreadable DLC(s); continuing with 82 accepted DLC(s)
```

## Validation plan (ARM64 DevRelease)

1. Launch the base game and update **without any DLC**; confirm both still work.
2. Launch with known-good DLCs. No `Rejected DLC` messages.
3. Launch with the Xenoverse 2 bad DLC included. Observe rejection in
   `emulation.log` and that the guest reaches the game's menu.
4. Repeat with bad DLC at start, middle and end of the input list and
   verify only that DLC is skipped.
5. Verify empty/no-CNMT NSP, non-DLC CNMT, missing referenced data RomFS,
   metadata-only entitlement DLC, and large/out-of-bounds `ListAddOnContent`
   offset and undersized output buffer.
6. Confirm that base/update failures still abort and that the known
   `PresentationEngine` teardown issue is tracked independently.

Do not merge until an ARM64 build succeeds and the device results are checked.
