# Dragon Ball Xenoverse 2 — DLC RomFS startup diagnosis

## Scope

This temporary diagnostic patch investigates the failure to launch with a DLC NSP
for add-on title ID \`010078D000F8900A\`. In the supplied reproduction,
83 DLC file descriptors are selected and startup stops in \`NCA::BuildRomFsBacking()\`
with \`LoaderResult::ParsingError: Invalid resolved RomFS header\`.

The original Android bugreport also showed a subsequent \`SIGABRT\` when
\`PresentationEngine\` is destroyed while its presentation thread remains joinable.
That teardown defect is independent and **not fixed by this diagnostic PR**.

## Instrumentation (no behavior changes)

- \`OS::Execute\` reports the one-based DLC index before/after every NSP load.
- \`NspLoader\` reports the failing .nca filename, load mode, result code and exception text.
- \`NCA::BuildRomFsBacking\` reports only on an invalid RomFS header: NCA title ID,
  section index, encryption type, selected key route (**rights-title**, **key-area**,
  or **none**), key generation, presence of a rights ID, IVFC data level/offset/size,
  resolved size, BKTR/compression table sizes, first 16 **RomFS header** bytes,
  and the parsed header/metadata offsets.

No key, title-key bytes, ticket, or rights ID value is logged.
The NCA/RomFS validation and error propagation remain unchanged.

## Reproduction

1. Build the ARM64 DevRelease APK from this PR, leaving all other settings unchanged.
2. Launch the same Xenoverse 2 base game + update with the same 83 DLCs enabled.
3. Send the new \`emulation.log\` after the failure.
4. Locate \`DLC probe: opening X/83\` without a corresponding \`DLC probe: opened X/83\`;
   correlate it with \`NSP NCA probe\` and \`NCA RomFS invalid header\`.

If the first header fields are nonsensical, inspect decryption and the NCA
section/IVFC mapping without assuming a bad key. If the header fields look correct
but validation fails, inspect the exact rejected constraint. The previously
observed CNMT-vs-NCA hash mismatch is independent evidence of an altered or
inconsistent content file, not by itself proof of the decryption failure.

## Validation and safeguards

- Check Android compilation with the existing \`PR Build\` workflow.
- Keep failures fatal; do not skip a malformed DLC or bypass header validation.
- Confirm startup without this DLC still reaches the normal game flow.
- Do not merge the probe into master before it is removed or explicitly accepted.
