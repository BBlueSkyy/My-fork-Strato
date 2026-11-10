# Experimental bounded mixed authority — independent PR on #400

Base: PR #400 checkpoint `a448ad06e84f0edd193520c08d17e5d1b819e56f`.
The original PR #400 and production `master` are not modified by this branch.

## What is activated
- **Only the `dev` Android flavor** defines `STRATO_EXPERIMENTAL_MIXED_AUTHORITY=1`.
  It enables the existing `MixedRegionAuthorityEnabled` gate at **compile time**.
- `mainline` and builds without the define retain the production default **OFF**.
- The existing eligibility checks remain unchanged: fully resident staged buffers,
  native reversible RGBA8 UNORM/sRGB, optimal 2D, one backing layer/mip/sample,
  complete proved regions and documented endpoint/resource limits.
- The first 16 successful mixed registrations emit
  `[Mixed experiment] admitted ...` in `emulation.log` (not per draw).
- This branch does **not** add an in-game toggle or broader format support.
  Restart using the other APK to switch the experiment off.

## Critical safety boundary
The existing manager registration paths **throw on unsupported buffer/image
physical overlap**. They deliberately do not proceed with untracked legacy
access. The B2 guest-dependency withdrawal fix protects the supported old
resources after withdrawal; it does **not** make an arriving incompatible peer
safe to execute. Thus this APK is a **fail-closed smoke experiment**, not a
generally usable or crash-free selective-admission implementation. A crash on
unsupported overlap is possible and **must not** be treated as proof that the
mixed-authority engine is broken or as permission to ignore the rejection.

In particular: direct memory import (DMI), sparse, nonreversible authoritative
images, unproved layouts, arbitrary mip/layer use and general B1b physical
allocation reuse are **not supported**. Turn DMI **OFF** when testing.
No automatic fallback, ignored-overlap path, general resource GC or new GPU
synchronization protocol is introduced.

## Build and test
The experiment-specific `push` trigger reuses the original ARM64 DevRelease
PR Build workflow; artifact: `strato-mixed-experimental-arm64-release.apk`.
Only this branch's `dev` build has the compile-time switch enabled.
The PR is intentionally based on `feat/resource-sequencing-stage1-20261025`,
not `master`, so it shows just the experimental delta.

1. Keep the original #400 APK for A/B comparison. Back up save data. Stop
   emulation before switching APKs.
2. Install the experimental ARM64 DevRelease; keep DMI disabled and preserve
   driver, Free Guest Texture Memory and game scene between comparisons.
3. Test a short SpongeBob Cosmic Shake scene where textures have previously
   been normal/corrupted. Note whether the log includes
   `[Mixed experiment] admitted`. **No admission means the experiment did not
   exercise the mixed authority path**.
4. Record screenshots, the emulator log, driver and Free Guest state. If an
   unsupported-layout exception occurs, stop the test and attach the log;
   do not attempt to bypass the rejection or treat it as a working fallback.
5. Then check a small regression set (Mario Kart 8, Odyssey, Zelda EoW).
   Any observed change requires A/B confirmation with equivalent settings.

The experiment alone **does not establish** that the SpongeBob corruption is
caused by buffer/image synchronization. No Android device test or performance
improvement is claimed here; CI only builds and runs its existing tests.

## Exit criteria
Keep this PR Draft, separate and unmerged. Do not enable the switch in the
original #400 or `master` based solely on an APK that builds or boots.
Further changes to support incompatible resources require separate content
proof and a safe admission/disposition design.
