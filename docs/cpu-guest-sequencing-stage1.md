# CPU guest texture synchronization — stage 1

Base: `master` at `9935f133`. This experimental branch **selectively reuses the verified implementation and host tests from Draft PR #392** (`7c42375a`). It does not import #393/#395/#396 or claim a new general-purpose resource sequencer.

## Problem established in master

`Texture::SynchronizeGuest()` could publish `Clean`/`CpuDirty` and clear `memoryFreed` for an ungrouped GPU-dirty texture **before** its GPU-to-guest transfer completed. A CPU read/write trap could then access unrefreshed or discarded guest bytes. `CopyToGuest()` also reversed `memcpy` source and destination for single-level linear textures.

## Stage-1 behavior

- Keep the guest copy invalid and the state lock held until a successful readback; reject concurrent traps by retrying outside the trap mutex.
- Commit `dirtyState`, `memoryFreed`, and `gpuReadbackSerial` only after copy/fence success; preserve retryable GPU ownership on failures or unsupported conversions.
- Preserve valid retained guest bytes for host-converted, sampled-only textures without pretending a GPU write occurred.
- Correct the linear destination copy, preserving block/pitch paths and layer iteration.

## Validation

Run `python3 tests/gpu_guest_readback/run.py` for the production synchronization/trap callbacks, race, failure/retry, and alias cases. Run `python3 tests/gpu_sampled_layout/run.py` for production `CopyToGuest` layout/layer cases. These tests passed on PR #392; their execution on **this branch** and Android gameplay/FPS have **not** been independently verified.

## Important non-goals / next phases

- This is **not** the developer's complete CPU-side resource sequencing implementation. No new global scheduler, resource graph or GPU sequencing is introduced.
- Partial physical overlap authority (A→B→A), arbitrary GPU texture dimensions, TIC/descriptor cache invalidation, and guest unmap/remap lifetimes are not solved here.
- Free Guest Texture Memory's eviction policy and `MADV_REMOVE` behavior are unchanged. Safe page-discard eligibility is separate (#395).
- Do not assume this resolves UE4 HDR corruption or does not impact FPS. Compare identical Release APK/settings in affected games before considering a merge.
- Future work should establish a minimal, deterministic failing producer/consumer ordering case in the **existing** CPU/guest access paths before adding another scheduling mechanism. Prefer resource-local dependencies; avoid global GPU waits or per-draw full-cache scans.
