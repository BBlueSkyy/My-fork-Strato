# PR #341: controlled mip-selection experiments

**Laboratory, not a proposed graphics fix. Do not merge.**

The user tested #447 on Moto G100: MK8's exaggerated pink/white glow remains.
Quick-bind dependency refresh is therefore not a sufficient explanation or
visual correction. This experiment does not include #447 and does not modify
#400, #444 or #445. All arms use master `505bc7d1` and the same probe code.

## Historical comparison and dependency analysis

`381d52d3` is the first parent of `3e8689f6` (the #341 integration).
Inspection of the complete parent/integration diff and internal PR history
separates these mechanisms:

| Mechanism | Before #341 | After #341 |
| --- | --- | --- |
| Exact/full reuse | Looser dimensions/view assumptions; no physical backing bound/growth check | Checks dimensions, image type, layer stride, range, and whether compatible native mips can grow |
| Nonzero mip lookup | Mip/byte counters advanced only inside an address-equality branch; most nonzero mips unreachable | Internal commit `46747bb5` advances counters for every mip |
| Tile configuration admission | Requires declared parent/request TileConfig equality | `46747bb5` also allows a single-mip block-linear request whose **effective per-mip** dimensions/GOBs/size match the parent mip, even when declared TileConfig differs |
| PromoteBackingForView | Format admission only; copies existing mip/layer count | Can add native mips, upload CPU-owned old data first, GPU-copy old mips, initialize new mips from guest bytes and invalidate old native-view generations |
| Separate incompatible layouts | Legacy overlap handling | Exact-mapping alias groups maintain writer/generation, reconcile guest bytes and reject obsolete cached aliases; this is distinct from several mip views of one Texture |

Internal history: `39761926` introduces growth/range guards; `e168bd6d`
introduces alias groups, followed by portability, ownership, trap and transfer
fixes; `46747bb5` adds the two mip-admission changes above. Reverting growth
alone would invalidate assumptions used by the new view guards/aliases, so
this experiment does not remove safety checks or create out-of-bounds views.

For the observed native 320×180/160×90/80×45/40×22/20×11 chain, 4 bytes per
pixel and effective Y GOBs 16/16/8/4/2 give physical mip sizes
327680/81920/20480/6144/2048, total **438272 bytes**. A standalone mip0
allocation is 327680 bytes. Those mappings do not satisfy the identical/full
mapping condition for the growth path. That excludes growth **for this specific
pair of descriptors**, not every possible bloom creation history. A directly
created five-mip backing also needs no promotion. The common probes record
CREATE/FULL_REUSE/MIP_REUSE, PROMOTE, alias registration and reconciliation
to establish which paths actually execute on the phone.

## The three arms

| APK label | Only behavior difference from A | Purpose |
| --- | --- | --- |
| **A_control** | None in GPU behavior; common bounded probes and lab app identity only | Confirm master still reproduces the glow under these probes/settings |
| **B_strict_tile** | Require `sameTileConfig` at the final validated mip admission | Test dependence on the relaxed declared-layout admission added in #341; effective geometry and range validation remain |
| **C_base_mip_only** | Require `level == 0` at the physical mip-boundary match | Test dependence on nonzero-mip reuse made reachable by #341; no change to offset arithmetic, full reuse, mip growth, aliases or barriers |

B and C are **independent** alternatives to A. C does not include B.
No global bloom toggle, brightness/gamma clamp or shader replacement is used.
These policy changes are deliberately experimental and may cause other graphics
to regress. They are not candidate production fixes.

Skipping a parent mip sends lookup down the existing no-match path. That can
create independent images with stale/guest contents. Therefore disappearance
of the glow **alone** is not proof the original view was wrong: the experiment
may instead have broken the bloom input. The value of the A/B is to identify
which mechanism changes the symptom; then prove the exact wrong range/data
before implementing a separate minimal fix. Do not merge B/C as a workaround.

The lab branch contains no changed runtime source files. `prepare.py` validates
their SHA-256 values and the complete `app/` baseline, then generates the shared
read-only probes and one policy predicate on the runner. Every artifact includes
`runtime.diff`, `manifest.json`, and the APK's SHA-256. Ordinary local builds of
the branch do not apply any experiment; use the explicit preparation command.

## Exact data recorded, bounds and limits

- `PR341_AB_VARIANT`: binary policy label, once on manager construction.
- Up to 512 manager/texture events for the native **320×180 B10G11R11UfloatPack32**
  backing: requested physical mapping, dimensions/GOBs, parent mip count,
  selected mip/layer, creation, promotion, alias registration/reconciliation.
  B and C log rejections only at a concrete mip/layer boundary where all control
  dimension, GOB, range, stride and candidate-preference checks passed.
  A later overlap candidate or depth-slice priority can still change the final
  selection; correlate rejected candidates with A's final MIP_REUSE and views.
  PROMOTE/ALIAS_REGISTER/RECONCILE are labeled **attempt**; a later exception can
  prevent completion. A CREATE event reports allocation, before upload/use.
- Up to 128 draw reports, first 16 then one per 128 matching shader draws:
  **05EB6F31815A3BF8** and **F8F8901DE46A8AFF**, actual resolved source/target
  TextureView ranges, format/swizzle, render extent and planning-stage masks.
  Each report logs at most 32 sampled and 32 target entries.
- Blend factors plus **guest register** constants are logged. Those constants
  are the intended draw state, not a capture of Vulkan's recorded dynamic state;
  a blend-constant fault is not assumed or claimed. Null views are handled.

No GPU readback, forced submission, new fence wait, upload, image/view creation
or alias synchronization is added by the probes. Source/target pointer equality
does not establish same-subresource feedback. Draw records reflect CPU planning
order, not exact GPU/present completion. Logging changes timing; this is not an
FPS benchmark. Budget saturation/absent events cannot prove a path never ran.

## Test procedure and interpretation

1. Use the same MK8 update/save, Turnip driver, resolution and settings on all
   three APKs. Start with Free Guest Texture Memory **OFF**. All use the same
   separate `.pr341lab` package, displayed as **Strato PR341 Lab**. Set it up
   once, then install B/C as updates without clearing lab data. The existing
   master/Dev installations are preserved. No capture flags are needed.
2. Cold boot **A**, enter Mario Kart Stadium, save a screenshot/short recording
   and `emulation.log`. Repeat for **B**, then **C**, then **A** again. Keep track
   of which APK is installed; verify its `PR341_AB_VARIANT` line.
3. Report glow/white flare, other broken effects and stability for each arm.
   If A does not reproduce, the experiment is inconclusive; check settings and
   instrumentation timing before interpreting B/C.
4. B-only change implicates declared/effective tiling admission; C-only change
   implicates nonzero subresource sharing; both changing the symptom points to
   their common reuse/contents dependency. This is localization, not yet proof
   of a faulty line. Neither changing it directs the next isolation toward
   actual observed growth/alias paths and the exact compositor inputs.
5. A rejection must be matched to the bloom backing and recorded draw views.
   No hits means the variant did not exercise that intervention in the recorded
   segment. Check logs before concluding the tested mechanism was excluded.

The first requested device result is MK8. SMO/BOTW/RE5 are acceptance checks
for a later proven production correction; B/C may intentionally regress them.
The separate menu/fog fixes of #444 are not included in these master-based APKs.

## Local verification and build

`python3 experiments/pr341/test.py` compiles each **actual generated selector**
under ASan/UBSan, exercising five captured bloom mip geometries, exact offsets,
equal-allocation wrong dimensions and wrong-GOB rejection. It verifies only
the declared policy axis differs and that common probes match across arms.
Concurrent counter tests verify the trace budgets. These are host metadata
tests, not complete FindOrCreate/Vulkan/game execution.
With the pinned Vulkan-Hpp/headers and fmt includes available, the same script
also compiles and executes the actual generated probe bodies against host
metadata facades (including null views/aliases). CI requires those dependencies;
this still does not compile the complete Android translation units locally.

Prepare an isolated clean checkout with:

```sh
python3 experiments/pr341/prepare.py --variant A_control --output pr341-artifact
./gradlew --no-daemon --stacktrace --parallel --configure-on-demand --build-cache assembleDevRelease
```

Preparation refuses unpinned/already-prepared sources. For manual individual
builds, use a separate checkout per variant. The dedicated PR workflow instead
uses `build_all.py` to build A/B/C sequentially in one checkout, restoring only
the generated source files between arms and reusing native/Gradle caches. All
three APKs therefore use the same test signing key; the build verifies that
their signing certificate digests match, as well as checking AArch64 ELF native
libraries and the expected variant marker in the native binary (to catch a
stale incremental APK). Signature compatibility is checked within one workflow
run; a future fresh run can generate a different test certificate.
Artifact subfolders A/B/C contain each APK, diff and manifest, plus
an overall `results.json`; an unrelated arm's failure does not discard successful
APKs. The lab application ID/name changes are identical in all arms.
Compilation and device
results remain pending until actually run. The user asked **not to monitor CI**;
opening this Draft triggers builds, without polling their status.
