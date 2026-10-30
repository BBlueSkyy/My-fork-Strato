# Resource-first preparation and short regional publication

This continues local checkpoint `f60370d1` without discarding its physical-region ledger, masked readback, compatible GPU copies or original nine PR #400 commits. The old checkpoint report is historical; its operation-spanning mutex is replaced here.

Follow-up: a separate cold-lookup retry loop introduced by the regional checkpoint is reproduced and corrected in [cold-region-lookup-retry.md](cold-region-lookup-retry.md). Device startup causality remains unconfirmed.

## Protocol and ordering

1. `AttachTexture` acquires/initializes the resource and collects regional uses of the current operation. It retains no regional mutex. Resource, view and descriptor resolution, read barriers and boundary callbacks can finish without introducing component → resource lock ordering.
2. Immediately before publication, `AcquireRegionPublication` takes the already-acquired resources' state mutexes in address order, then distinct regional mutexes in address order. It checks CPU dirtiness and every resident version against the current authority version. A changed input raises the existing submit/reconcile/retry request before any new render-pass node or writer ownership is published.
3. The guard covers combined render-target/storage claims. It excludes CPU callbacks only during this short publication scope and ends before query hooks or later resource preparation. Compute uses the same validated publication guard. No full resource locks are acquired inside it; repeated image-view access refers to the already-locked and preflighted resources.
4. Retained read-only storage views are preflighted too, including the no-descriptor-update and partial-update graphics paths. The collector visits only this operation's regional resources; ordinary textures do not enter it. No texture-manager/cache scan is added per draw.
5. A prepared regional host snapshot carries `pendingRegionUseTag` until submission/unlock. If the CPU writes after preparation, `SynchronizeHostInline` keeps that later guest state CpuDirty rather than replacing the earlier pending GPU read's host rows. The next use encounters invalidation, submits prior work and reconciles before consuming the newer state. This also preserves foreign GPU-owned rows whose guest bytes are still old. Tagged writers are GPU-dirty under the publication guard, so their write callbacks already require the resource lock before relinquishing ownership.
6. Reconciliation continues to snapshot donors, acquire full resource locks without metadata held, then recheck component identity and the entire current donor set under metadata protection. A newly selected donor restarts acquisition; its old cycle cannot stand in for its queued producer.

Generic operation retries clear collected uses before resolution. They do not retain component locks or fabricate writer claims. The protocol does not provide general rollback of arbitrary allocations after nodes/writers have already been published; that broader executor transaction is outside this stage.

## Deterministic evidence

Host setup: Clang 18, Vulkan/lavapipe, synchronization validation. Vulkan-Hpp/Boost development dependencies were extracted outside the repository. No Android toolchain or workflow dependency was changed.

- `TEXMAN_LOCK_ORDER_PROBE=1 python3 tests/gpu_backing_promotion/run.py`: passes both image-format-list modes. Real resource contention runs presentation's resource → component path while executor waits on the presentation texture. The same test was RED on the checkpoint; it no longer relies on suppressing that wait.
- `TEXMAN_AUTHORITY_PROBE=1 python3 tests/gpu_backing_promotion/run.py`: 0/1024 stale texels, exclusive halves, alternating writers and masked publication, FreeGuest OFF/ON × image-format-list OFF/ON. ASan/UBSan also pass with leak checking disabled for the Vulkan driver.
- Changed-donor schedule: reconciliation waits on A; B becomes a new owner while still locked by its executor with an unsubmitted blue Vulkan clear. Acquisition must restart and wait on B's resource. Only that wait allows B's actual submission/cycle assignment. The final image contains A's red rows and B's newly produced blue rows, with zero Vulkan validation errors.
- CPU mutation or a newer GPU writer between attachment and publication rejects the prepared writer before its serial/ownership advances; retry consumes the newest real Vulkan bytes.
- A CPU callback cannot change authority through the short publication guard. It can run during general preparation and after the guard, so the previous long lock is not simply renamed.
- Late CPU write with foreign GPU-owned shared rows: the earlier GPU read keeps both its old exclusive bytes and fresh shared GPU rows; the next use sees the later CPU byte and still preserves the shared rows. No GPU completion wait is added for this snapshot handling.
- Complete production `AddSubpass` and `CreateRenderPassWithSubpass` bodies are compiled against graph/engine facades and real texture resources. A boundary callback changes retained read-only storage data; the rejected attempt adds no new render-pass nodes or writer. The draw hook confirms metadata is unlocked. This checks control flow and publication, not execution of a complete Vulkan render pass/guest shader.
- Full `tests/gpu_backing_promotion/run.py` passes: exact aliases, failure recovery, cached views, formats, mip expansion, common mappings and producer ordering. Essential submission, texture-sequence, sampled-layout, storage-preflight, overlap-materialization, draw-texture and framebuffer-cache suites pass. No check is silently skipped or made mandatory in CI.

Both data and concurrency gates are required. A passing A→B→A alone is insufficient. The first checkpoint's lock-order failure and the invalid padded-eviction fixture remain documented in [the historical report](region-authority-checkpoint.md).

## Scope and performance limits

Regional transfers remain restricted to proven compatible dense native 2D block-linear rows, including supported physical mapping fragments, mips and layers. Incompatible transitions drain safely into the existing paths. General buffer/texture ownership, arbitrary padded/incompatible transfers, sparse overlaps and complete remap lifetime management remain pending.

No #392/#399 readback corrections, shader clamps, NaN substitutions, broad #393 graph or private Strato implementation were imported. FreeGuest coverage checks are cached at setup and shared regional guest memory is retained; padded layouts can therefore consume more host memory. At draw/dispatch publication, the common path has no regional allocation, regional lock, manager scan or additional GPU wait; publication work scales with this operation's actual regional uses. FighterZ FPS and Android driver behavior still require device measurement.

The APK ARM64 DevRelease (Release) workflow is unchanged. No RelDebug, new required CI test, merge or Actions monitoring. SpongeBob, Sparking! ZERO, Bulletstorm and FighterZ have not been declared fixed on a device.
