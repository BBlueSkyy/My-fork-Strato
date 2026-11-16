# MK11 memory growth: opt-in diagnostic build

Base: master `650aae297c93510fd119dffb39d0e329602dc008` (includes merged scheduler PR #362).
This change is diagnostic only. **No GC, eviction, cache policy, Vulkan barrier, scheduler, shader, or resource-lifetime change.**

## Motivation and reproducer

Moto G100: Strato's FPS overlay uses `/proc/self/statm` RSS. User reports MK11 increasing from ~5 GB to >7 GB after repeated fight > character selection > arena transitions, accompanied by worsening FPS. Both maximum GPU clocks and the readback hack affect performance and must be kept constant across A/B comparisons.

## Enabling / output

Only this PR branch builds with `-PgpuMemoryDiagnostics=ON` in CI. Other branches and default local builds use OFF. Manually build with `-PgpuMemoryDiagnostics=ON assembleDevRelease` if needed. APK artifact is `strato-pr<PR_NUMBER>-arm64-release.apk`.

While the emulator submits Vulkan command buffers, **at most one sample per 15 seconds** is logged under `[GPU-MEM]` in `emulation.log`; the first successful submission triggers the initial sample. No new thread or polling timer; no per-frame logging, and no logging in normal builds.

Each line contains:

- `clock_ms`: monotonic clock for relative alignment with captures.
- `rss_kib`, `anon_kib`, `file_kib`, `shmem_kib`, `swap_kib`: kernel process memory from `/proc/self/status`, KiB. RSS includes shared pages; it is not proportional set size (PSS).
- `vma_alloc_kib`, `vma_blocks_kib`, `vma_allocations`: totals across Vulkan Memory Allocator heap budgets. *Allocated* denotes live allocation sizes; *blocks* denotes memory managed/reserved by VMA, including unused space.
- `textures_live`, `textures_created`, `textures_destroyed`: constructed/destroyed Texture wrappers, including guest and host variants. Not image bytes, mappings or alias-group counts.
- `staging_live`, `staging_kib`, `staging_created`: live StagingBuffer wrappers, requested sizes, and cumulative allocations. Staging is already included in VMA totals; **do not sum these**.
- `megachunks_live`, `megachunks_mib`, `megachunks_created`: pooled megabuffer chunks; 25 MiB requested per chunk and already included in VMA. These are not per-frame live allocations.
- `sample_us`: time spent collecting proc and VMA statistics, to catch expensive snapshots.
- `snapshot_failed`: diagnostic exceptions are contained; no synchronization/eviction policy changes.

VMA counters exclude direct-imported allocations and may not capture all driver-side allocations. RSS includes guest memory, Java, CPU caches, and some GPU-backed mappings. Differences between RSS and VMA do not establish a leak. No unsafe live scans of texture-manager vectors or alias groups; those require locks and could disturb sequencing.

## Device measurement protocol

1. Install this branch's diagnostic APK. Keep driver, resolution, readback hack, maximum GPU clocks, shader cache state and settings constant across tests. Record which are enabled.
2. Boot MK11, wait ~15s for a snapshot, record idle/menu FPS/RSS.
3. Enter one fight and wait ~15s. Return to character selection; wait ~15s. Repeat 5-10 fights, including same-arena and different-arena rounds. Record elapsed time and stage per transition, plus FPS.
4. Export the full `emulation.log` and compare ordered `[GPU-MEM]` samples. Repeat same sequence on current master using its RSS overlay to establish baseline.
5. Diagnose: persistent `textures_live` + VMA allocated growth suggests retained image-backed resources; `staging_kib` growth suggests staging caches/in-flight owners; increasing `megachunks_live` shows pooled megabuffer high-water retention; RSS growth without these suggests guest/host CPU memory, direct imports, shader/pipeline caches or driver-side allocations.

**Causation is not proven by correlation.** A fence may retain staging after cache eviction, a cache can grow legitimately to a steady state, and VMA block capacity may remain after live allocations fall. The next step after logs is a bounded attribution and a targeted fix in a separate PR. Do not merge this diagnostic into master as-is.

## Second-pass ownership diagnostics

After the first device-lost capture, the live texture count reached 7,115 with zero destructions. To distinguish wrapper growth from mapping retention, the next diagnostic build also emits:

- `mapping_entries`: number of guest mapping entries inserted into `TextureManager::textures`. The manager retains `shared_ptr<Texture>` in these entries. No entries are removed in the inspected master implementation; this count is cumulative, not a number of unique textures.
- `mapping_guest_kib`: sum of guest virtual mapping spans inserted, **not unique physical memory** and **not Vulkan allocation size**. Overlaps and aliases can double count.
- `replaced_mark_events`: count of first-time `replaced=true` assignments at the three known `FindOrCreate` sites; this is a lower bound for stale representations, not a unique global retired-texture count and not evidence of safe eviction.

No scans of live texture vectors, no locking added to the hot path, no reclamation, no changes to alias lifetime. These counters can support attribution but **cannot by themselves establish whether any texture can be freed safely**.

The first capture ended with `VK_ERROR_DEVICE_LOST`; capture a full log even if only 1-2 arena transitions are possible. Compare the rates of `mapping_entries`, `textures_live`, and `vma_alloc_kib` before and after a return to character selection.
