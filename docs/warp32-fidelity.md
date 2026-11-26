# Warp32 fallback fidelity follow-up to #430

The shader-compiler audit is at
`app/libraries/shader-compiler/docs/warp32-fallback-audit.md`. It documents every
requested operation, exact cases, retained approximations, tests and inherited
supported-stage validator limitations.

The only runtime semantic change is a true shuffle validity predicate when a
compile-time proof across all 32 guest lanes establishes that the scalar data
result is also exact. No lane mapping, dynamic ballot/mask emulation, shader/game
special case, texture, TIC or RenderDoc change is introduced.

Device regression testing on Snapdragon 870 / Adreno 650 reported no apparent
regressions in Zelda: Echoes of Wisdom, Luigi's Mansion 2 HD, Pac-Man World Re-Pac,
The Patrick Star Game and NFS Hot Pursuit Remastered. The PR workflow also validates
the Warp32 oracle, SPIR-V fallback modules and the existing regression suites.
