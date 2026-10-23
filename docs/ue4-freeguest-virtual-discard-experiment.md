# Virtual FreeGuest experiment — diagnostic only

This **must never be merged into master**. It builds upon diagnostic
PR #388, with unchanged ownership tracing.

When **Free Guest Texture Memory = ON**, `Texture::FreeGuest()` still
marks the guest memory as logically evicted (`memoryFreed=true`), and
`SynchronizeHostInline` still chooses `GpuDirty`, but Android does
**not** call `MemoryManager::FreeMemory(mirror)` (`MADV_REMOVE`).

This isolates actual physical page discard from the GPU/guest ownership
and readback policy. The log uses
`TEX_OWNER event=logical-free-physical-pages-retained` instead of
`free-guest-pages`.

### Reproduction

Use the same driver, game, shader cache state, scene and settings as the
**Free Guest Texture Memory ON** run of #388, with
**Fast GPU Readback Hack OFF**. Launch the **Release APK** of this
experimental PR, leave Free Guest Texture Memory **ON**, then provide a
screenshot of the UE4 texture corruption and `emulation.log`.

Compare against the earlier #388 ON run:

* **Same improvement:** physical `MADV_REMOVE` is not required for the
  observed partial improvement; ownership/readback transitions are stronger
  candidates.
* **Improvement disappears:** the actual memory discard (or a downstream
  side effect of retaining guest pages) matters.
* **Other differences:** neither mechanism is isolated conclusively; inspect
  per-texture `TEX_OWNER` events and sampled hashes.

This is a deliberately artificial diagnostic mode; `memoryFreed=true`
does not describe the actual physical page state. It is **not a valid
production optimization**, and the result does not prove corruption's
underlying cause. Do not benchmark performance with this APK.

No new workflow and no RelDebug; use the existing PR Android
`assembleDevRelease` build. Keep the PR Draft and do not merge.
