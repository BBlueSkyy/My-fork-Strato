# Session B2 — persistent dependencies after mixed withdrawal

## Scope and state

Started from remote `2eefb90687d7ce114dc2cc3e8f0cc21399e43f6a`, master
`f0a95dd80eb150e81d16e1b0fa88c3a812a820c8` and recovered local checkpoint
`fdebbe431f4bb3824035ed869a90168babbf3e1b`. Earlier commits, sRGB guest bridge,
CBUF, domain cache and master integration are retained. User device baseline:
checkpoint APK had no reported perceptible difference/regression on Moto G100.
No game-specific fix or new device measurement is claimed.

`MixedRegionAuthorityEnabled` remains **false**. This is a bounded transactional
withdrawal increment, not completed Stage C or global/selective activation.

## Lost dependency and replacement

Previously, mixed `DrainRegionAuthority` published current owners and cleared
all resource links. A later buffer writer then returned early from
`RecordRegionWrite`; the retained image was neither invalidated nor repaired.
The Session B reproducer found 4096/4096 stale shared bytes after the next write.

`RegionLink` now represents one of two immutable roles behind **one atomic
shared-pointer load/store**:

- Active regional authority, exposed by `active()`.
- A distinct `GuestDependencyDomain`, exposed by `dependency()`, after withdrawal.

`load()` supplies either tracking role to the existing executor, callbacks,
producer publication and repair hooks. `regionTracked` continues to mean byte
coherence is required; it does not mean active mixed admission. The cached
`regionMixed` bit still arms deferred writer preparation for shared resources in
both roles, and is independent of transport eligibility.

The replacement owns its own physical cells, typed last-writer and guest versions,
and weak resource memberships. Bindings preserve cell indices/resident versions.
It uses the already validated selective guest bridge. It does not own or refer
to the original ledger. Tests require empty active links, a different replacement
identity, and expiration of a weak pointer to the old ledger before the future
writer test succeeds.

This is not a GC: no global registry/sweeper is added. Resources/retained views
own their tracking link; the domain owns only weak resource references. Old
snapshots may keep the predecessor alive until they finish/retry. With no resource
or operation references left, ordinary shared ownership releases the metadata.
Structural registration/migration updates the live graph; metadata remains bounded.

## Transaction

For mixed groups, `DrainRegionAuthority` now:

1. Expands membership and requests submission/retry for attached uses.
2. Acquires full buffer/image locks, then state locks, in established order.
3. Validates the bounded staged/native/full-coverage domain and allocates every
   replacement cell/member container before any publication. Revalidates closure
   after blocking acquisition; a changed closure requests a fresh preparation.
4. Publishes necessary owners under the **old** masked authority, without regional
   or trap locks during GPU waits. The old graph remains authoritative if this fails.
5. Locks old metadata in order, requires every copied cell guest-current, copies
   current history into prepared storage, then atomically replaces resource links.
   All participant resource/state locks cover the multi-resource commit. Commit
   performs no allocation, GPU wait or callback.

Failures preserve the old graph and writer claims. Valid publication already
completed under that graph may monotonically advance guest versions; it is not
rolled back into stale guest data. This is bounded resource-transition recovery,
not general rollback of arbitrary executor work or externally visible writes.
Image-only drain retains its earlier detach behavior.

Later supported registration preserves the dependency role rather than silently
reactivating mixed admission. Coalescing preserves role, ownership and physical
intervals through the existing migration path. Incompatible materialization,
legacy image-alias conversion and backing-footprint changes cannot silently
remove a dependency domain; those paths reject until separately proven.

## Additional preparation race reproduced and fixed

Mixed registration expanded its previous groups **before** blocking resource
acquisition. A withdrawal could replace that group and a new writer advance the
successor while the new consumer waited. Registration then inherited stale
predecessor cells: 4096/4096 obsolete bytes, no retry.

Post-acquisition validation now requires all actual groups to match the expanded
snapshot, every previous group still to be used, and every live member to belong
to the acquired resource closure. Otherwise the existing reconciliation request
unwinds before partition/writer publication and prepares again. The deterministic
concurrent byte test now reports 0/4096 obsolete bytes and exactly one retry.

## Content and failure evidence

Clang18 host Vulkan fixture, lavapipe, ASan+UBSan and Khronos synchronization
validation. Free Guest OFF/ON and both format-list modes where applicable.
No instrumented production hook or new mandatory CI workflow was added.

| Gate | Result |
|---|---|
| Original withdrawal target, actual buffer/image writes and pending reader | RED 4096/4096 before implementation; final 0/4096 after original ledger release |
| Test-only omission of persistent replacement | Expected assertion failure with 4096/4096 stale bytes; production gate unchanged |
| Allocation/command-record/queue-submit failures during withdrawal | Old active graph, versions and writer serial retained; cancelled cycles accounted for; retry succeeds |
| Cancellation after preparing a withdrawn buffer writer | No writer serial/node publication; later actual operations succeed |
| Future Buffer/Image writers, late consumer and CPU reads | Byte-exact, including exclusive intervals and guest publication |
| Coalescing after withdrawal | New backing retains dependency role; retained delegates/offsets and bytes correct |
| GMMU remap before/during preparation | Old epoch invalid; new translation Q and real DMA writes remain separate from retained physical P; exclusive/guard bytes correct |
| Concurrent CPU fault during withdrawal | Prepare blocks outside trapMutex; old snapshot restarts against successor; actual CPU bytes and later guest/image contents correct |
| Registration waiting across withdrawal and newer GPU producer | RED 4096/4096; final 0/4096 with one preparation retry |
| Existing automatic mixed/bridge/coalesce/rollback/drain suite | Pass, zero synchronization-validation errors |
| A→B→reuse A authority gate | Four 0/1024 stale-texel results, Free Guest OFF/ON |
| Lock-order probe | Pass in both format-list modes |
| Sampled-layout and alias gates | 20 oracle upload/readbacks plus alias suite pass |

Evidence log names retained beside the checkout:
`session-b2-red.log`, `session-b2-negative.log`, `session-b2-snapshot-red.log`,
`session-b2-snapshot-green.log`, `session-b2-release-modes.log`,
`session-b2-release-automatic.log`, `session-b2-sampled.log`.
The focused review examined the transition and post-lock validation only; it
reported no concrete blocker and did not run tests. Root ran the listed gates.
No domain-cache benchmark was repeated: stable lookup remains the existing
single atomic operation, and no per-draw scan/allocation was added.

Reproduction:

```
source ../test-env.sh
ASAN_OPTIONS=detect_leaks=0 LP_NUM_THREADS=2 SANITIZE=address,undefined \
TEXMAN_MIXED_WITHDRAWAL_BOUNDARY=1 python3 tests/gpu_backing_promotion/run.py
# Focused event and registration-race modes:
# TEXMAN_WITHDRAWAL_EVENTS=1
# TEXMAN_WITHDRAWAL_SNAPSHOT=1
# Temporary negative control (must fail):
# TEXMAN_MIXED_WITHDRAWAL_BOUNDARY=1 TEXMAN_WITHDRAWAL_LINK_NEGATIVE=1
```

## Bounds and remaining admission limits

Withdrawal admits only existing fully resident staged buffers and native optimal
RGBA8 UNORM/sRGB 2D, one mip/backing layer/sample, complete-allocation layouts
already proved by the bridge. It is bounded by 128 images, 128 buffers and 4096
cells. Existing registration/bridge retry and endpoint limits remain. No new
format, layout, sparse or DMI transport is admitted.

The remap tests cover translation-association P→Q changes with retained physical
allocations. They do not establish general physical allocation deallocation/ABA
reuse (B1b), Android imported-memory visibility, or a protocol allowing an
incompatible authoritative image to execute through the withdrawn component.
The incompatible peer stays rejected; the old supported resources stay coherent.

Before experimental selective activation, prove the remaining ordinary-incompatible
arrival/disposition policy. Do not equate persistent old-resource dependencies
with permission for an untracked peer to consume/write shared memory. DMI, sparse,
non-reversible writers, oversized domains and unproved footprints remain excluded.
The global switch cannot be enabled on the strength of this withdrawal test alone.

Android SDK/NDK is absent locally; actual changed production bodies compile in the
host fixtures. One existing ARM64 DevRelease PR synchronization build is requested
only after publication; no Actions completion monitoring or CI investigation.
No merge, independent PR changes, new game work or Session C starts here.
