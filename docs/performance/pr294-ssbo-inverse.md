# Teste SSBO INVERSO — PR #294

Experimental only. Not a permanent correction. Do not merge.

## Confirmed device evidence and next question

User reports restored Zelda Link's Awakening performance on Moto G100 using
9da6fda2c55110dfe01142cb0f09a90d666fbcfc: bad c7cb28d with only the old compiler
and SSBO contract restored. This implicates that group in the tested context;
it does not identify has_size, raw pointers, fallback bounds or compute inference
individually. Numeric benchmarks and full graphics acceptance were not supplied.

This INVERSE build has exactly good 28bcb957a9f018212f3b0a2e5628028bdb22bf69 as
its parent and adds only the shader compiler/SSBO group:
- app/libraries/shader-compiler: dcb938866f4234963fcbc199f2443438d8ef6f07.
- GetStorageBufferBinding and StorageBufferGuestDescriptor from c7cb28d.
- Maxwell StorageBufferDesc metadata and constructor from c7cb28d.
- Compute SyncDescriptors access_range consumption and groups signature/call.
- Device maximumStorageBufferRange property required by that binding.

The pipeline.inc tail after GetTextureBufferBinding is byte-identical to GOOD;
there is no wholesale replacement of pipeline.inc. Compute dispatch receives
only the groups argument, without the #294 retry wrapper. All texture/view,
promotion, retry, framebuffer, RenderArea, macro and remaining execution sources
are exactly GOOD. No #401/#404/#405/#406 code, new options, FPS hacks or logging.
Restoring GOOD texture behavior here is an isolation experiment, not a proposal
to remove compatibility fixes from master.

The existing pr_build.yml is restricted to this branch's push event, checks out
the exact event SHA, compiles assembleDevRelease only, verifies parent/compiler
pins and uploads a labelled APK plus SHA256/provenance. No PR merge checkout.
CMake flags, Gradle build settings, ABI and package ID are unchanged from GOOD.

## Local validation

Source checks compare binding and metadata against c7cb28d, verify the unchanged
non-SSBO binding tail and dispatch logic, and whitelist every changed path.
The actual dcb93886 descriptor and access-range structure compile with the actual
Maxwell consumer under UBSan. Sized/raw metadata is preserved (not fabricated);
affine compute range, unknown range and zero-size dispatch cases passed.
These checks do not replace the Android build or real game validation.

## Device comparison and interpretation

Use the existing good 28bcb957 and bad c7cb28d APKs, restored-group 9da6fda2, and
this inverse APK. Zelda 1.0.0; Moto G100 SD870/Adreno650/Android12; Turnip 24.0.99;
same location/route, resolution and options, including readback and clock options.
Compare settled gameplay, not shader compilation startup. Record approximate FPS
and confirm objects, shadows, reflections/effects, UI and transitions remain intact.
Shader cache initialization counts are not evidence of persistent FPS regression.

If the inverse reproduces the loss while complete graphics remain, the new group
is sufficient in the good baseline. Only then create narrower compatible compiler/
host experiments to separate (1) sized classification of legacy candidates,
(2) newly recognized 8-byte pointers, (3) unknown 8MiB fallback and (4) compute
access-range inference. Observe the actual stage/cbuf/offset/classification and
requested/effective ranges of Zelda bindings before designing the final solution.
If the inverse stays fast, investigate interaction with the other #294 groups;
do not conclude that the compiler is irrelevant or revert it globally.

Any permanent correction must preserve raw-pointer and Marvel support and be
implemented on the latest master only after isolation. PR #404 is untouched;
there is no correction PR or merge from this experiment.
