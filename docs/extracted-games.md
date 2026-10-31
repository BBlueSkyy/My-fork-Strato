# Extracted Switch games in regular ROM folders (experimental)

Any ROM directory already selected in Strato can contain an extracted, decrypted
game alongside NSP/XCI/NRO/NSO/NCA files. No extra folder selection or copying
to `Android/data` is needed:

```text
<selected ROM folder>/
  Zelda/
    exefs/
      main
      main.npdm
      rtld
      sdk                    # optional
      subsdk0                # optional
    romfs/                   # optional
      ...
    control.nacp             # recommended, optional
  other-game.nsp
  game.xci
```

Subfolders are scanned recursively as before. When a folder contains the three
required ExeFS files, it is shown as **one game** in the library. Refresh the
game list after adding extracted content.

## Android Storage Access Framework (SAF)

The game-root directory URI retains the persisted tree grant from the ROM folder
chosen by the user. Strato enumerates the ExeFS and RomFS document tree on the
emulation thread, gathers relative paths and sizes, and opens document file
descriptors **on demand** through ContentResolver. Native code maintains a
bounded LRU of 24 open descriptors and uses `pread64` for random reads.
Unlike a copied or rebuilt NSP, file contents stay in the selected ROM folder.

The existing `NcaLoader::LoadExeFs` process path handles the NSOs. A virtual
RomFS index uses the existing `LayeredRomFsBacking` and the SAF file backings.
Program ID comes from `main.npdm`; existing ExeFS/RomFS mods remain available.

The previous experimental app-owned `switch/extracted/` location is still
recognized for backward compatibility, but **is not required**.

## Requirements and limitations

- Files must be **decrypted**, and the ExeFS folder must contain `main`,
  `rtld`, and `main.npdm`. An encrypted `*.nca` dump is not an extracted
  ExeFS game.
- Prefer a real `control.nacp` at the game root: it contains language, save
  sizing, ownership and runtime settings. If missing, minimal metadata is
  generated from NPDM; unknown save sizes cannot be reconstructed reliably.
- Arbitrary external NSP updates are not applied to an extracted base until
  full base/update content reconciliation is implemented. Use a properly
  resolved extraction or packaged NSP/XCI for that scenario.
- SAF providers must expose byte sizes and seekable file descriptors.
  Providers that only offer streams or omit sizes are unsupported.
- The virtual metadata is created from the file list, with a maximum of
  200,000 files. Recursively enumerating many folders may have noticeable
  startup latency (it runs off the UI thread).
- Real device performance, booting and APK compilation still require validation.
