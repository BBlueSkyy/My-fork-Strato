# Teste SSBO — PR #294

Experimental isolation only. Do not merge as a permanent fix.

Base: c7cb28d822c52437ed83e9f86ed15c3a3b77c6e7.
Restored shader compiler: 6c804797a6e073c925958cedd6d5eb8114a3c65d.
Reference good: 28bcb957a9f018212f3b0a2e5628028bdb22bf69.

Only the shader compiler gitlink and its SSBO consumers change in production:
- GetStorageBufferBinding is restored byte-for-byte from the good reference.
- Maxwell StorageBufferDesc is restored byte-for-byte from that reference.
- Compute's storage-buffer descriptor writer is restored byte-for-byte.
- No fabricated has_size value, access-range inference, fallback size, or new
  classification heuristic bridges the old compiler to the new host contract.
- The old compiler emits its original descriptor model; this is a historical
  experiment, not a claim that arbitrary raw pointers have a valid size field.

All other production sources, including the remainder of pipeline.inc (the
storage-image usage check), texture promotion/retries, framebuffer, RenderArea,
MME, memory and command execution are identical to c7cb28d. The compute groups
parameter remains as an unused API argument so dispatch and its retry wrapper
need not change. The restored compiler already has uses_render_area metadata.
No #401/#404 instrumentation, #405/#406 optimization or master GPU changes are
incorporated. PR #404 and master are not modified.

The existing pr_build.yml is scoped to push events on this experimental branch,
checks out the exact event SHA (not a synthetic PR merge), builds only
assembleDevRelease, and uploads the APK plus a SHA256/provenance text. Other
workflows remain unchanged. No changes to CMake flags, Android Gradle config,
package ID, game settings or performance options.

Host checks passed: exact source comparison, protected-file diff whitelist,
absence of has_size/access_range consumers, old compiler pin and compilation
of the actual old producer descriptor with the actual Maxwell consumer under
UBSan. Full Android integration is verified by CI, not those host tests.

Device acceptance remains pending: Zelda Link's Awakening 1.0.0, Moto G100,
Turnip 24.0.99, same region/route, resolution, graphics and readback settings for
all three APKs. Compare the good 28bcb957, bad c7cb28d and this experiment.
Check all objects, shadows, effects, UI and transitions; fewer rendered effects
must not be interpreted as recovered performance. Initialization pipeline
counts do not diagnose sustained FPS loss. Use shaders generated for each
variant when inspecting generated code; do not compare first-compile stutter
against warmed gameplay or attribute the regression to cache counts.

If performance recovers with complete graphics, the next experiment is good
plus the new shader/SSBO group, then sub-isolation of has_size classification,
8-byte pointers and compute access limits. If it does not recover, this group
alone is insufficient and image/matching/promotion/framebuffer/RenderArea/MME
isolation follows. No inverse APK is produced until this device result exists.
