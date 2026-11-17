# Homebrew bundle installation (Android)

The **+** icon on the main screen installs a Nintendo Switch homebrew bundle into
Strato's emulated SD card. It does **not** convert NROs to NSPs, modify game
RomFS/ExeFS, or install a separate Android APK.

## Import workflow

1. Select the **folder** containing an `.nro` using Android's folder picker
   (e.g. the extracted `supermarioworld` folder, not `Nswitch`).
2. The installer requires **exactly one NRO** in that folder and recursively
   copies the complete directory tree. This preserves files such as
   `smw.ini`, `smw.sfc`, `smw_assets.dat`, shaders, audio and subdirectories
   without hardcoding game-specific filenames.
3. Files are first copied into a hidden staging directory beneath
   `getExternalFilesDir(null)/switch/sdmc/switch`. Only after a successful
   copy is that directory renamed to a collision-free installed directory.
   Existing installations are **not** overwritten.
4. Strato refreshes the game library, which already discovers NROs inside
   the emulated SD card. It launches them from an actual filesystem path, so
   `ResolveNroGuestPath` can supply `sdmc:/switch/<folder>/<file>.nro`
   through Homebrew ABI argv[0]. Standard libnx derives the working directory
   from this argument.

The installer checks document entry names for path traversal/separators,
rejects more than one NRO, and sets reasonable recursion and entry limits.
It deletes staging files on failure. It does not delete the source folder.

## What this does not do

* It does not generate game data or supply ROMs. Assets must already be in the
  selected folder, and their expected names and formats are defined by the port.
* It cannot fix an underlying libnx/Horizon filesystem, SDL, graphics, or service
  compatibility bug. A homebrew can still fail after it is installed.
* It does not install a launcher icon on Android, and it does not implement the
  Nintendo Switch's official title installation semantics.
* Install folders with multiple NROs individually rather than selecting an
  entire collection.

## Manual validation on Android

- [ ] Install a folder containing one NRO and two sibling files.
      Check that all three appear under the emulated SD card.
- [ ] Install a folder with nested files/subfolders; confirm structure is preserved.
- [ ] Confirm it appears in Strato's game library without adding its parent
      directory to the ROM search locations.
- [ ] Launch it, inspect `emulation.log` for the `NRO launch path: sdmc:/...`
      line, and check that it can open relative files.
- [ ] Install the same folder twice: existing install must be preserved,
      and the new one must receive a different name.
- [ ] Select a folder with no NRO and a folder with two NROs: both should
      give a useful error and leave no installed entry behind.
- [ ] Interrupt an install and refresh the library: incomplete NROs must not
      be listed.
- [ ] Test a plain NRO from an existing ROM search directory: its previous
      loading behavior must remain unchanged.
- [ ] Test a regular NSP/XCI title: its existing loading behavior must remain
      unchanged.

### Super Mario World

The desired source folder includes `smw.nro`, `smw.ini`, `smw.sfc` and
`smw_assets.dat`. The NRO's error *"without the rom file"* is also displayed
when its internal SNES initialization fails, so **that message alone does not
prove the .sfc is absent**. If it remains after bundle installation, capture
`emulation.log` and determine whether the NRO launch path, libnx cwd,
asset lookup, or SNES ROM loading failed before making changes to the emulator.
