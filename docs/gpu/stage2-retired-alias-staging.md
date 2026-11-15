# GC Stage 2A — audited retired-alias download staging

Audited starting commit: `57da90ccf05afafdac8781e04a1a9241d80dcab6`.
Continue only PR #457; independent of #441. No texture eviction or staging pool.

## Audit results and safety corrections

The ordinary successful path holds sorted Texture locks during registration and
waits for each download copy before reading its staging on the CPU. Trap paths
try-lock the same Texture; cached staging is private to it. Historical retirement
occurs only after authoritative guest encoding succeeds. Eligible siblings retain
their caches. Texture images/views and their existing GPU dependencies are not
collected by this policy.

Two assumptions needed corrections before expanding diagnostics:

1. `ReconcileAliases` checked `aliasRetired` before waiting for writer/target locks,
   then could clear `aliasStale` after another registration retired or reassigned
   that target. A deterministic two-thread test reproduced that revival. Under
   the final target/state locks, publication now rejects retirement or a changed
   alias-group identity **before** touching dirty state or guest-memory validity.
   `SynchronizeGuest` also explicitly rejects retired representations independently
   of the stale bit. Writer checks, generations and retirement eligibility remain
   unchanged.
2. A failed fence wait does not prove completion. The original readback staging
   had no independent ownership on its copy cycle. An injected post-submit wait
   failure followed by retirement reproduced premature staging destruction.
   Readback now prepares its fence dependencies **before** submission: staging and
   the source Texture (when it still has a shared owner). It records that copy in
   `Texture::cycle` before waiting. A failed wait leaves those dependencies intact;
   subsequent backing changes/readbacks order that copy. Pre-submit preparation,
   recording or submission failure cancels the unused cycle and rethrows.

The last-owner destructor also performs a final synchronous readback. It cannot
use `shared_from_this()`; `weak_from_this().lock()` intentionally returns empty
there. Staging remains attached and the destructor still waits synchronously.
Destructor wait failure remains terminal as before, not a new recoverable device-
loss/teardown guarantee. Normal scheduler `Wait(true)` clears the temporary
Texture/cycle ownership loop. No new worker, queue, wait-idle or fence poll.

The readback change follows Strato's existing explicit pre-submit retention in
backing promotion/grouped uploads; no incompatible external-emulator collector is
ported. Vulkan wait errors include memory-allocation failures, so ownership is
not based on assuming all failed waits mean the device has stopped using memory.

## Conservative expansion decision

No additional permanently unusable staging class was demonstrated:

| Candidate | Decision |
| --- | --- |
| Permanently retired historical alias | Drop cached download reference after successful registration. |
| Eligible stale/clean alias or current writer | Keep: it can become authoritative and need more readbacks. |
| Read-only materialized alias | Existing readback guard prevents download-cache allocation. |
| Growing mip backing | Already drops obsolete-size download cache; instrument that existing release only. |
| Generic replacement, age, memory pressure | No new eviction: reuse/authority is not proved absent. |

Accordingly expansion is limited to diagnostics and regression coverage. No
changes to Free Guest Texture Memory, alias authority selection, NCE, guest
semaphores, shader cache, global resource sequencing or other PRs.

## Diagnostics: cache ownership, not physical GPU memory

`texture::StagingRetirementStats` belongs to one GPU/session. Identity is set before
guest execution. Relaxed atomic counters update only on nonempty cold-path drops:

- `buffers`: cached staging references discarded;
- `cached_bytes`: cumulative **requested capacity** of those caches;
- `operations`: nonempty batches (registration may discard several buffers).

`[GPU-GC2A] event=ready` identifies game/title once. `event=cache_drop` logs the first
and every 16th nonempty batch, with cumulative counters and reason `retired_alias`
or `obsolete_mip_size`. No per-frame/draw logs, clocks, scanning or per-readback
counter updates. Diagnostic logging exceptions cannot fail committed retirement
or prevent startup. Concurrent snapshots are approximate, not transactional.

Capacity dropped is **not** a measurement of VMA heap memory, RSS, or completed
`vkFreeMemory`. Pending fences/other shared owners may retain the same allocation;
VMA may also keep freed blocks internally. An absent drop event does not prove
that a game never read back textures; its active caches are intentionally retained.

## Validation and reproducible evidence

- `python3 tests/gpu_sampled_layout/run.py`: ASan/UBSan; retirement vs concurrent
  reconciliation RED→GREEN, replacement-group revalidation, existing atlas layout,
  overlapping-alias routing and future-writer invalidation checks.
- `python3 tests/gpu_staging_retirement/run.py`: standalone production counter
  implementation, batched bytes/buffers/operations, per-game identity, empty drops,
  sparse reporting and 4,000 concurrent updates. ASan/UBSan.
- `python3 tests/gpu_backing_promotion/run.py`: exact production texture/manager/
  copy bodies on real Lavapipe Vulkan with Khronos core/synchronization validation,
  both image-format-list modes; zero validation errors. Tests cover:
  - Failed-wait staging expiry reproduced before the retention fix.
  - A test-only timeline semaphore holds the actual copy fence **not ready**;
    retirement and local Texture/cache-owner destruction do not free the staging
    or source until the semaphore is signalled and fence dependencies are cleared.
  - Staging allocation, dependency attachment, command allocation (the harness
    `failRecording` switch) and submission faults preserve retirement flags, content, cache and counters.
  - Repeated readbacks reuse the same live cache; a retired representation cannot
    reallocate it even with a stale stale-bit snapshot.
  - Existing mip growth drops only obsolete-size staging, with matching counters;
    allocation failure preserves the old image/metadata. Existing all-mip,
    six-layer/cubemap, render-target, alias, CPU-contention, replacement/destruction
    and byte-content checks remain enabled.
- Existing `gpu_submission`, `gpu_null_storage_image`, `gpu_texman_multirange`,
  `gpu_queries`, and `gpu_draw_texture` suites pass.
- Workflow YAML and required gate order checked; stats and real-Vulkan lifetime
  suites now precede the existing `assembleDevRelease` step. The current variant
  requests native Release, filters `arm64-v8a`, verifies nonempty AArch64 ELFs and
  uploads `strato-pr457-arm64-release.apk`.

The initial real-Vulkan fixture failed before this work: its freed-writer direct
call selected a layout whose guest backing was deliberately discarded, while the
production manager supplies an explicit requested layout. Its expected bytes also
included unrecoverable non-image layer padding after FreeGuest. The fixture now
passes an explicit requested layout and expects zero discarded padding while
still verifying all encoded image content. Four old clean-state probes allocated
unused command cycles without cancelling them; they now cancel before teardown,
matching production lifecycle. No production guest-memory policy was changed to
make these tests pass. `TEXMAN_TRACE=1` optionally names test cases for diagnosis.

Local host dependencies: existing Clang 19, bundled Vulkan-Hpp/headers, Mesa
Lavapipe 25.0.7 and Khronos validation layer 1.4.309. The test-only timeline gate
requires timeline-semaphore support; Android gains no timeline feature requirement.
ASan uses `detect_leaks=0` because LeakSanitizer cannot inspect this container's
ptrace environment. This is not a full renderer leak or ThreadSanitizer proof.

Local Android attempt: `./gradlew --offline --no-daemon assembleDevRelease` stops
before project compilation because Gradle 8.2 is uncached and its download returns
`java.net.SocketException: Network is unreachable`; local SDK/NDK is unavailable.
A local APK build is not claimed.

## Performance, limits and delivery

The cache release may call `vmaDestroyBuffer` on the registration/growth thread
when it was the last owner. VMA uses its existing internal allocator synchronization;
no new allocator lock is added. Driver free cost, contention and frametime stalls
remain unmeasured on Qualcomm/Turnip. The necessary readback lifetime fix adds
fence dependency nodes/shared ownership to download operations. It introduces no
additional copy submission or blocking wait on the successful path, but no claim
of zero CPU/FPS regression is made. Device FPS/frametime and prolonged-session
memory measurements are still required.

This recovers only retired cached download capacity, not texture images or the
whole renderer's memory growth. No additional eviction boundary was invented.
Update the same Draft PR, never merge, and request its existing ARM64 Release
workflow by branch synchronization. Stop without monitoring that build.

### CI dependency correction

Run `43741705762` stopped before Android assembly because the new native Vulkan
gate could not find `boost/container/small_vector.hpp`. The runner did not have
the Boost development headers that the local host fixture used. That gate now
explicitly installs `libboost-dev` before compiling. No GC/runtime code changes.
