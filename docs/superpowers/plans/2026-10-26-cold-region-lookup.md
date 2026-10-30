# Cold regional lookup retry correction

Goal: fix the demonstrated non-progress retry introduced after baaf3cd2 without weakening resource/callback safety. Scope is the SpongeBob startup regression investigation; device causality remains unconfirmed.

Design: keep the resource-first regional precheck. At a cold regional registration/drain retry boundary, retain the manager and requested GuestTexture in the existing control-flow request. After Submit releases the executor's resources, resolve that lookup without its context tag, publishing the cache entry/domain transition before restarting the interrupted operation. The next attempt reuses the entry instead of reconstructing the same aborting registration. Ordinary lookups and reconciliation are unchanged. Removing the precheck would reintroduce cross-resource lock inversion; repeating Submit alone has been proven unable to make progress.

- [x] Add a bounded test-only retry oracle around production RunWithTextureBackingRetry / AttachTexture / FindOrCreate / retry handler, using real Vulkan images.
- [x] Demonstrate baaf3cd2 manager succeeds and 45452491 manager repeats registration/drain, with the current safe publication protocol held constant.
- [x] Carry only cold regional lookup work across the existing retry boundary and execute after submission/unlock.
- [x] Verify compatible/incompatible lookup, existing-component drain, lookup failure/recovery, and no new work for cached/no-overlap uses.
- [x] Rerun Vulkan authority 0/1024 ON/OFF, lock/donor/CPU callback and essential regression suites.
- [x] Document exact static/test evidence and device uncertainty. The only shippable correction is the proven non-progress retry; startup success still requires device validation.

Constraints: preserve the nine baseline commits plus both regional commits, all safety checks, ARM64 DevRelease, no global waits, no shaders, no new sequencing system, no Actions monitoring or merge.
