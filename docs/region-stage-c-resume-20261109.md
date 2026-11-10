# Stage C continuation — 2026-11-09

Stage C is **not complete**. `MixedRegionAuthorityEnabled` stays **false**.
The working release gate is unchanged; the automatic mixed tests enable only a
temporary header. No phone performance or game-specific fix is claimed.

## Recovery and master integration

| Item | Confirmed state |
|---|---|
| Initial remote PR400 | `dfa18b4348fe39b950761acd448792552846a647` |
| Initial remote master | `f0a95dd80eb150e81d16e1b0fa88c3a812a820c8` |
| Recovered cache checkpoint | `refs/checkpoints/stage-c-domain-cache`, `897528ab00ec52ca43bfecaa2091c28942dddc7b` |
| Full pre-integration WIP snapshot | `refs/checkpoints/stage-c-pre-master-20261109`, `5e3bd9f6fd7a2def398df14f1bee9a9e1770d134` |
| Recovered overlay including validated CBUF guard | `refs/checkpoints/stage-c-overlay-20261109`, `704723009df391c5efdbf9db53719aab05e3eba9` |
| Reviewed/performance bridge snapshot | `refs/checkpoints/stage-c-bridge-review-20261109`, `6a1b5e7b824e2ba64457d3738d268a3489f0d189` |

The original dirty checkout and every earlier checkpoint were preserved before
integration. Recovered work contained the classification cache, CBUF consumer
RED, a pending bridge content test and its design. It did **not** contain a
functional guest bridge or a completed old cache review. Both were continued
from that actual state rather than reported as recovered implementations.

Master was merged into the feature history in `663a536e` (parents dfa18b43 and
f0a95dd8). Five textual conflicts were resolved by combining typed TIC caching,
B1a receipts/retention, null policy and deferred writers; no complete-file
ours/theirs replacement was used. #428 invalid-DLC isolation and #430 null
Vulkan descriptors, backing/view layers and subgroup stage handling are retained.
The shader compiler is exactly master's `a987e2a5c8fa507c805d5867737de5976c7c2ce0`.
See [integration details](region-master-integration-20261109.md).

The integration also fixed an independently reproduced semantic conflict:
regional coverage must name all backing layers, even when a view exposes only
one. Real Vulkan RED found 4,096 hidden-layer and 8,192 guest byte mismatches;
GREEN found zero for both format-list modes. View ranges remain separate.

`bff72fb1` preserves dfa18b43's tolerance for idle unmapped CBUFs during global
refresh, but actual full/quick graphics and compute descriptor consumers require
a mapped bound slot. Truly unbound slots retain their existing dummy behavior.
The regression checks each real consumer before any dummy allocation and reads
new bytes after remapping. It does not suppress the original selector exception.

## Coverage of the bounded guest bridge

| Case | Before continuation | Current implementation / boundary |
|---|---|---|
| UNORM whole-GOB mixed resources | Direct GPU transport behind disabled gate | Preserved |
| Native RGBA8 sRGB, staged buffer | Mixed registration rejected | Persistent guest-bridge transport, byte-range writers and resident versions; automatic tests only |
| Direct group gains eligible sRGB peer | Rejected | Transactional replacement preserves old histories, views and pending uses; future writes keep invalidating consumers |
| Partial buffer writes / exclusive bytes | Proven in direct domain | Bridge uses selectively published owners, then the existing target upload; own unpublished exclusive cells are published first |
| CPU faults with foreign donors | No bridge mode | Donor preparation outside trap/metadata locks; callbacks validate without waiting |
| Coalescing a bridge buffer | No bridge mode | Keeps transport, typed owners and resident versions; old backings retained for pending commands |
| Padding / stride gaps / edge GOBs | No proven mixed fallback | Explicitly excluded from bridge admission by full-coverage proof; old direct/image-only paths preserved |
| Mips / multiple backing layers / 3D | Existing image regional support, narrow direct mixed domain | Not newly admitted by bridge |
| DMI / nonreversible images / sparse | No safe general mixed disposition | Still hard boundaries; global activation remains unsafe |

The raw benchmark files preserve their original local commit IDs. The published
merge/CBUF/cache trees are identical to local 422441d5/d4ea5b3c/048a37a4; only
GitHub commit identity changed during publication. The after measurement is the
retained review checkpoint, before setup-only bound tightening. No benchmark
was repeated for the final metadata test.

The initial bridge domain requires fully resident native optimal single-sample
RGBA8 UNORM/sRGB, one mip, one backing layer, non-interleaved 2D block-linear
layout and complete representable allocation coverage. Each image has at most
128 whole GOB spans. A bridge component has at most 128 images, 128 buffers,
4096 cells and 8192 preparation endpoints; retry loops stop after 64 attempts.
Exceeding a limit preserves the previous graph and rejects the attempt; it is
**not** a completed unsupported-resource fallback.

Guest transport is independent of `regionMixed`. An immutable group discriminator
selects the bridge. Only latest owners in the target footprint are acquired and
published; image guest publication remains masked by byte-range authority.
A stale target uploads from canonical guest bytes after those owners are current,
then validates resident versions and retries if producers changed. This may need
a whole *private* target staging/upload in the bounded domain; guest publication
never overwrites unrelated or foreign-owned cells. Stable resources perform no
bridge transport. No global GPU idle was added.

## Cache and performance

`3190dbfc` publishes the recovered `regionMixed` classification cache after an
independent lock/lifetime review and integrated sanitizer/Vulkan gates. It is
protected by existing resource locks, set by all registration/drain paths and
separate from transport mode. CPU trap callbacks do not read this cached bit.

Reproducible command (before cache versus integrated bridge review snapshot):

```
source ../test-env.sh
python3 tests/gpu_resource_benchmark/run.py --ref bff72fb1 --activate-mixed --iterations 1000000 --output before.json
python3 tests/gpu_resource_benchmark/run.py --ref refs/checkpoints/stage-c-bridge-review-20261109 --activate-mixed --iterations 1000000 --output after.json
```

Host: AMD EPYC 9V74, Clang18.1.3, O2, measured control thread pinned to CPU0,
2 lavapipe workers. Seven samples per case; sanitizers are tested separately.

| Case | Before ns (format-list OFF / ON) | After ns (OFF / ON) |
|---|---:|---:|
| Unshared image writer | 12.909 / 13.116 | 11.593 / 11.130 |
| Unshared image prepare+commit | 41.782 / 42.050 | 40.750 / 39.705 |
| Shared image writer | 106.735 / 108.666 | 93.865 / 93.531 |
| Shared image prepare+commit | 297.259 / 300.243 | 281.280 / 269.166 |
| Valid image reconcile | 17.300 / 17.578 | 18.301 / 18.315 |
| Unshared buffer prepare | 18.627 / 18.382 | 19.400 / 18.448 |
| Cached mixed buffer prepare | 92.634 / 94.142 | 96.303 / 96.001 |
| Cached mixed image prepare | 142.387 / 145.784 | 144.967 / 142.721 |

Every stable case still reports zero allocations, copies and barriers; cached
mixed tests additionally assert no extra trap changes, staging allocations,
submissions or waits. A required 4096-byte image repair uses one copy call,
two barrier calls and zero GPU-completion waits. Counters are from actual host
Vulkan hooks, not FPS telemetry.

The initial 10,000-operation pairs had timing variation, including +6–8% in
unshared preparation. Increasing sample duration and excluding concurrent test
compilation removed that apparent preparation regression. Residual small-case
changes remain (valid reconcile about +0.7–1 ns; cached buffer +2–4%); the stable
return precedes the new bridge branch and has no new allocation/transfer work.
These measurements do not isolate code placement, scheduling or frequency, and
are not evidence of device regression clearance. Raw short/long JSON is retained
in [performance/region-20261109](performance/region-20261109).

## Remaining release gates

Ordinary DMI, unencodable or oversized peers can still enter a physical component
and make mixed registration reject. A persistent supported bridge does not solve
that general arrival/retirement protocol. Keep the production switch off.
Bridge-specific mapping races, donor replacement during upload, every cancellation
boundary, negative controls, CPU writes during publication, and device cache
visibility require further proof before activation. Existing direct-mode tests
must not be relabeled as bridge-mode coverage.

Full Android translation-unit compilation was attempted locally but stopped at
missing platform headers (`sys/system_properties.h`); no SDK/NDK is installed.
Actual changed method bodies compile and execute in the host production-body
fixtures. This is not a claim that an ARM64 APK compiled or ran locally.

Device baselines are unchanged: the user confirmed Odyssey gameplay after
dfa18b43. Gravity Circuit's shader read at 0x10000 predates this work. Neither
that issue, game-specific rendering/FPS, nor PR421 was investigated or modified.


## Final checkpoint validation — metadata exhaustion

The focused test `CheckMixedBridgeBudgetRecovery` reaches the actual production
4096-cell ceiling by splitting the buffer-exclusive cell into smaller valid
intervals with identical producer and versions. Image bindings retain their
indices. This is deterministic fragmentation pressure, not a substitute authority
implementation or a new production compactor.

The actual manager coalesce is attempted with an old GPU consumer still queued.
Budget rejection preserves group identities, source delegates, guest bytes,
writer serials, every host/guest/resident version and the queued command. Removing
only that synthetic pressure permits the **same** manager operation to succeed.
Real Vulkan readback verifies the old consumer and new backing, then a later
buffer write repairs the image while retaining its exclusive bytes.

- Focused GREEN: `TEXMAN_MIXED_BRIDGE_BUDGET_PROBE=1 python3 tests/gpu_buffer_image_sequence/run.py --automatic`, ASan+UBSan and Vulkan synchronization validation, both format-list modes.
- Negative control: additionally setting `TEXMAN_BRIDGE_BUDGET_NEGATIVE=1` removes only the guard from the temporary extracted source. The rejection/old-group assertion fails as expected. Production files and the release switch are unchanged.
- No production fix was needed after the recovered bound checks. Only the missing regression, its temporary negative control and documentation were added in this finalization session.

Previously completed cache review, benchmarks, CBUF, GMMU/B1a and master integration
results remain as recorded above; they were not repeated as new successes. The
integrated automatic bridge/coalesce/rollback/synchronization gate is rerun on the
final source/test tree. Android local compilation is unavailable because the
SDK/NDK is absent. A single existing PR Build synchronization event will request
ARM64 DevRelease after the validated checkpoint is pushed; completion is left to
the user, with no Actions polling or failure investigation in this session.

Next session is limited first to selective-admission design/proof: safe arrival
and exclusion/retirement of incompatible peers in an existing live component,
including pending readers/writers. Current hard rejection is not that protocol.
Keep DMI, sparse, nonreversible images and unproved layouts outside activation.
Then prove bridge-specific map-generation changes, donor replacement/CPU writes
during publication and the remaining cancellation/negative-control matrix before
considering any component activation. No such expansion starts in this session.
