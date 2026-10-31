# Extracted game directories (experimental)

Strato can discover decrypted Switch program directories placed under the app-owned files directory:

```text
<Strato public files>/switch/extracted/
  Example Game/
    exefs/
      main
      main.npdm
      rtld
      sdk                   # optional
      subsdk0               # optional
    romfs/                  # optional
      ... original RomFS paths ...
    control.nacp            # recommended, optional
```

Use the app's Android DocumentsProvider or another allowed file-transfer method to place these files in the Strato public files directory. The games are picked up on the next library refresh. This feature does **not** read arbitrary Android Storage Access Framework directory URIs; Android grants those as document handles, not regular host directories.

## Implementation

* The UI scans immediate children of `switch/extracted` and opens each `exefs/main` as a regular file descriptor. The native loader resolves the file descriptor and verifies the canonical directory is an immediate child of the app-owned extraction root.
* The standard `NcaLoader::LoadExeFs` path handles `main.npdm`, `rtld`, `main` and other NSOs, preserving existing NCE and module semantics.
* `LayeredRomFsBacking` constructs a RomFS view backed by the extracted files (metadata only, no full repack/copy of file contents). Existing base-backed LayeredRomFS behavior is unchanged.
* Existing ExeFS/RomFS game mods still apply using the NPDM Program ID.
* When `control.nacp` exists at the game root, it supplies the actual metadata, save sizes and language settings. Otherwise a **minimal fallback** is synthesized using the Program ID; unknown save sizes/policies are not invented. Such titles can have limited save functionality.

## Constraints

* This is for a **decrypted** game extraction containing `exefs/main`, `exefs/main.npdm`, and `exefs/rtld`, not a directory containing encrypted NCAs.
* Currently only app-owned folders are supported (not arbitrary folders picked using SAF); layouts are case-sensitive.
* External NSP updates against an extracted base are explicitly rejected rather than silently mixing content without an appropriate base NCA. Use an already-resolved extraction or an NSP/XCI base for update handling.
* Large titles with many extracted files still need device-level verification of startup latency and file-descriptor usage.
* There is no validated Android APK or on-device compatibility claim until CI and device testing pass.
