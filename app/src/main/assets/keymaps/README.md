# Keyboard maps

Unmodified 0x1000-byte keyboard maps from Ryujinx `KeyCodeMaps.cs`, revision
`1df6c07f78c4c3b8c7fc679d7466f79a10c2d496` (MIT; see LICENSE.txt).
Source: https://github.com/alula/Ryujinx/blob/1df6c07f78c4c3b8c7fc679d7466f79a10c2d496/src/Ryujinx.HLE/HOS/Services/Settings/KeyCodeMaps.cs

The Default map is Japanese. Command 7 changes its first byte to 1 for the
legacy ABI, as Ryujinx does; commands 9 and 12 keep the original map.
English US uses the international US map, matching Eden rather than Ryujinx's
UK fallback. No empty synthesized map or inferred header is used.
