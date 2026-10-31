# Switch 2 / Donkey Kong Bananza: stage-1 audit and guarded memory groundwork

Status: **research / experimental only**. This branch is based on Strato
`master` `b8593d8b2c2e9742a4f1f9edfaa935f653cd3663`. It is separate
from the extracted Switch 1 loader in Draft PR #407. Do **not** merge.

## References studied (2026-10-31)

- [Eden-NX2 Android](https://git.eden-emu.dev/moceure/eden-nx2-android),
  branch `nx2-android`: source and detailed README describing verified
  Donkey Kong Bananza work on one Snapdragon SM8850 + Adreno 840 phone;
  approximate 12–16 FPS with persistent black terrain and intermittent
  purple screen, and Android 13+ / 16GB RAM recommendations.
- [Takoroka/eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2):
  upstream experimental Switch 2 service, graphics and memory work.
- [Mars-Hermetto/monkey](https://github.com/Mars-Hermetto/monkey):
  independent Windows gameplay experiments, generally 5–8 FPS in 3D.
  Its README and `patches/eden-ns2-dkb.patch` are research references.
- [Switch 2 title namespace](https://www.switchbrew.org/wiki/Switch_2:_Title_list/Games):
  native application IDs use the `0x04` prefix. Bananza's program ID
  is `040074A01BF12000`. Note that some Switch 2 Editions of Switch 1
  games retain `0x01` IDs; the predicate here is intentionally **not**
  a universal Switch 2 detector.

## Architecture comparison: blockers before the game can boot

| Area | Eden-NX2 | Strato baseline | Portability |
| --- | --- | --- | --- |
| Extracted ExeFS/RomFS | Switch 2 loose loader, base/update/dlc | Dedicated Switch 1 proof of concept in separate PR #407, not merged | Must add a Switch 2-aware content resolver; no automatic cherry-pick |
| Guest memory | 12 GiB guest memory layout; 16 GiB heap region | 39-bit guest VMM with 6 GiB heap; SVC SetHeapSize accepts 64-bit size | Stage 1 adds a 16 GiB *virtual* heap reservation for native `0x04` titles only; **not** 12 GiB physical memory or complete layout |
| CPU backend | Dynarmic backend used above 4 GiB; newer atomic instruction decoding | NCE/native ARM64 with signal-backed memory and process hooks | Not directly portable; needs independent design/validation |
| HOS services | NX2 services such as `nx2:gsv`, `vi2`, codec commands | Switch 1 service registrations | Not implemented; inventing success stubs would hide faults |
| GPU | Newer native instructions, GPU macros, geometry; SSBO and bind changes | Maxwell GPU / existing shader compiler architecture | Requires shader-compiler + GPU cross-repo port; not implemented |
| DLSS/terrain | Temporary bilinear stand-in; destructible terrain still black | No Switch 2 tensor/geometry implementation | Not implemented |

## What actually changed

1. `loader/switch2_profile.h`: tiny, compile-time-tested native `0x04`
   Switch 2 program-ID predicate; excludes Switch 1 `0x01` and zero.
2. `NcaLoader::LoadExeFs`: selects profile from *parsed NPDM*, logs what
   is missing and passes it to the VMM. Switch 1 behavior is unchanged.
3. `MemoryManager::InitializeVmm`: reserves a 16 GiB guest heap **only**
   when that profile is active, within existing 39-bit VMM, replacing the
   standard 6 GiB virtual heap. It rejects an inconsistent non-39-bit
   profile rather than silently mis-mapping the process.
4. The default remains the old 6 GiB size, and every older caller (NSO,
   NRO) uses the optional argument's default `false`.

These changes are an independent Strato implementation inspired by publicly
documented high-level NX2 requirements. No Eden GPL source code was copied
into MPL-licensed Strato implementation files.

## Safety, compatibility and next milestone

- Reserved virtual address space is not committed physical RAM, but
  **guest access may still exhaust RAM**. A Moto G100 Snapdragon 870,
  12 GB RAM and Android 12 is below the tested Eden-NX2 device and
  recommended configuration. Do not imply 16 GiB heap fits physically.
- An increased region alone **does not boot Donkey Kong Bananza**.
  The master Strato loader does not know how to launch a loose Switch 2
  game; PR #407 handles extracted Switch 1 games only.
- No service, GPU, shader compiler, kernel ABI or driver semantics were
  replaced, and none of the existing Switch 1 defaults change.
- Validate ARM64 DevRelease compilation, the `0x01` classification,
  39-bit VMM region bounds, and runtime behavior on a genuine NX2
  NPDM. Never merge before that.
- The next technically meaningful step is loading/executing a properly
  extracted `0x04` title while separately diagnosing the **first missing
  CPU/service/GPU requirement**. This may require redesigning the CPU
  backend and acquiring additional RAM on the test hardware.

No packaged or decrypted game assets are stored in this repository.
