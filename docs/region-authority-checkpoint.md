# Local region-authority checkpoint — publication blocked

This is **unfinished local work**, based on the nine published commits of Draft #400 (`baaf3cd2`). It must not be pushed or used as a validated APK. The original branch, master base `9935f133`, Release workflow and published PR are unchanged remotely.

## Data-path result

`TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py` now asserts **0/1024 stale intersection texels**, exclusive bytes, reverse writer handoff and masked guest publication. It passes for Free Guest Texture Memory OFF/ON and Vulkan image-format-list OFF/ON. This is actual Vulkan/lavapipe image data with synchronization validation, not an ownership-only simulation. An additional ASan/UBSan run passes (driver leak checking disabled).

The unpublished implementation includes:

- Persistent partitions of physical CPU mapping spans. Each region retains host-writer identity/version and guest publication version; each image binding retains its resident version. Component extension/splitting carries existing versions forward. Multiple ordered physical mappings and dense compatible mip/layer subresources are covered.
- Row-correspondence admission: native, reversible 2D block-linear color, matching dimensions/format/tile geometry/mip and layer counts, no GOB padding inside image subresources, and complete compatible rows at all physical intersection boundaries. Self-aliasing mappings, sparse resources, arbitrary incompatible layouts and buffer/texture authority are outside this domain.
- Stale foreign GPU rows are copied image-to-image, ordered after predecessor submission and with source-reader cycles/lifetimes retained. Guest-owned rows use partial buffer-to-image uploads; exclusive unpublished GPU rows survive. Residency is committed only after successful queue submission. Valid rows are not copied again.
- Readback encodes privately and publishes only intervals still owned by that image. Failed readback retains dirty/eviction state and ownership for retry. This is a region mask; no #392/#399 readback-publication or Linear memcpy fix was ported.
- Tracked components disable guest discard. A cached setup-time coverage proof also prevents ordinary FreeGuest from deleting row/mip/layer/mapping padding that no GPU image can reconstruct. This follows #395's coverage principle selectively, without its host-memory API changes or per-FreeGuest layout scans. Padded layouts retain more guest memory; device memory/performance effects remain unmeasured.
- Incompatible transitions drain the current authority domain, mark cached hosts CpuDirty and advance descriptor invalidation before detaching. Mip growth drains tracked ownership before changing the represented footprint.
- Descriptor epochs, retained storage writer slots, full/quick binding updates and deferred operation writer commits. Clear and blit preparation no longer publish a writer before a later texture preflight can retry. Compute retains inline-capacity writer staging.

## Mandatory concurrency blocker

The attempted protection against CPU invalidation between descriptor/attachment preflight and writer publication currently retains a component mutex through the operation (`HoldRegionAuthority`, `regionOperationLocks`). **That design is unsafe.**

A deterministic counterexample uses two clean, compatible images A and B in one component:

1. Presentation owns B's resource lock (`PresentFrame`), then needs the component mutex in `B.SynchronizeHost()`.
2. Executor attaches A and retains the component mutex.
3. Executor attaches B and blocks on B's resource lock while retaining that mutex.

The order is executor **component → B**, presentation **B → component**. Neither can advance. Retry cleanup after an exception does not prevent a blocking acquisition that never throws.

Run `TEXMAN_LOCK_ORDER_PROBE=1 python3 tests/gpu_backing_promotion/run.py` with the same Vulkan environment. It currently fails with `LOCK ORDER PROBE: BLOCKED`. The resource-mutex host boundary detects the forbidden blocking acquisition and throws before the harness can hang, then releases both threads. Production AttachTexture, HoldRegionAuthority, RunWithTextureBackingRetry and Texture::LockWithTag execute unchanged; this test enables real resource locking in the view facade, unlike the older metadata-only attachment fixtures.

**Publication is blocked until this test passes without removing the CPU authority guarantee.** A short, ordered publication/preflight mechanism or a fully justified nonblocking retry protocol is needed. Simply dropping the guard reopens the CPU callback race; holding it over general preparation introduces the inversion above. No claim is made that the architecture is impossible to fix, or that a full GPU rewrite is necessary. No speculative workaround was published.

A second review finding was addressed locally: ReconcileRegions snapshots only required donors, locks target/donors, then revalidates both component identity and the current donor set under metadata protection. A changed donor restarts acquisition rather than reading an unlocked backing/old cycle. A dedicated adversarial donor-change schedule is still desirable before publication.

## Verification performed on this checkpoint

Passing:

- Required Vulkan authority oracle, four FreeGuest/format-list combinations, exclusive halves, alternating writers and old readback rejection.
- Vulkan allocation, attachment, recording, queue-submission and masked-readback failure/retry; unchanged backing/view handles; no repeated repair on valid reuse.
- Vulkan fragmented mappings, component extension and independent transitive writers; a new C write does not invalidate disjoint A or recopy A into B.
- Production CPU callbacks: metadata/resource try-lock contention, both owner-publication orders, CPU stores, and a fragmented B-exclusive fault that relinquishes shared cells while A still owns exclusive GPU rows.
- Retained sampled/storage views, production descriptor-epoch branch and writer commits, cached preflight, incompatible drain and dense mip/layer copies.
- `python3 tests/gpu_backing_promotion/run.py`: full existing alias/promotion/mip/format controls, zero Vulkan validation errors.
- `tests/gpu_texture_sequence/run.py`, `tests/gpu_submission/run.py`, `tests/gpu_sampled_layout/run.py`, `tests/gpu_storage_preflight/run.py`, `tests/gpu_overlap_materialization/run.py`, `tests/gpu_draw_texture/run.py`, `tests/gpu_framebuffer_cache/run.py`.
- `git diff --check`; workflows/build configuration unchanged.

**Failing:** `TEXMAN_LOCK_ORDER_PROBE=1 ...` as described above. Thus passing pixel/content tests are insufficient, and this implementation is not complete.

The broad Vulkan suite previously stopped at `CheckFreedWriterGuestBridge`: inferred-layout validation rejected an evicted writer before its existing restore path. Validation now follows restoration. The old fixture additionally expected recovery of random padding after deleting it; the positive eviction control now uses a fully recoverable dense writer, and a separate negative padded-layout control checks that padding is retained. Test-only command buffers allocated solely for no-op state checks are explicitly cancelled before teardown. No previously failing check was silently skipped.

Host boundaries remain allocator/mirror/eviction/trap scheduling and some descriptor/engine interfaces. This is not Android MADV_REMOVE testing, a complete Android build, a real presentation stress test, a shader-storage dispatch test, or a FighterZ FPS measurement. The failing lock-order test is a deterministic concurrency check, not a game reproduction.

## Next acceptance gate

1. Replace the unsafe operation mutex scope and make the lock-order probe green with presentation/resource contention.
2. Recheck CPU writes and latest-writer ownership across that new boundary; exercise a changed donor while resource acquisition waits.
3. Re-run the authority oracle and affected essential regressions, then review and publish to **the same Draft #400** only after these gates pass.

General GMMU remap lifetime handling, buffer/texture ownership, arbitrary padded/incompatible overlap transfers and complete per-operation allocation rollback remain outside this stage. No new HDR/NaN capture or shader workaround was introduced. SpongeBob, Sparking! ZERO, Bulletstorm and FighterZ require subsequent Release device validation; no graphical or FPS fix is claimed. No merge, RelDebug, mandatory CI job or Actions monitoring.
