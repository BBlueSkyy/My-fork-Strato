# One controlled range experiment

This is an experimental branch based on historical BAD c7cb28d, not a production fix or master rollback. Both arms use the same source and modern compiler dcb93886. Only the build-time STRATO_SSBO_RANGE_AB macro differs. No compiler, Maxwell, SPIR-V or Eden experiments are repeated.

A keeps current SSBO sizes. B narrows only the readonly Compute cbuf0:280 binding of the previously captured module to AlignUp(4608 + address padding, host alignment). Certification requires its exact final SPIR-V SHA256 c0ce2cdf82978d808d47b49e81cab3dc8c2cef96b9a73f85dbd737ea28f50441. Shader code, pointer classification, unknown fallback elsewhere and synchronization stay unchanged. No certificate means no treatment and no valid negative result. A changed driver/profile/replacement must not silently receive this bound.

The existing complete static proof (not re-run here) covers the 16-byte final load at31*144+128. This is NOT a global cap or a permanent hash-specific fix. It is not applied to the other unknown shader at290. Marvel receives no range change.

## What was missing from existing files

Fully read build good.log (365 lines) and build bad.log (779 lines): no CBUF or SSBO observations. The prior compile capture has only CBUF1:0/4/8/C. Earlier SsboTrace/Dirty/Cycle logs expose modern requests, views and buffers, never runtime words0:288/2A8. Shader/pipeline hashes and physical backing lengths do not encode those values. Pipeline compile caches store queried compile-time words, not a full runtime CBUF0 snapshot.

For each target descriptor the experiment reads address+adjacent u32 in one16-byte read in BOTH arms. Slot0 means cbuf0:280 and legacy_word=CBUF0:288. Slot1 means2A0/2A8. It does not interpret the adjacent word as a valid size for the modern binding.

GOOD computes padding=P&(alignment-1), requests AlignUp(legacy_u32+padding,alignment) and clamps to the remainder of its first GMMU mapping; zero legacy word returns a4KiB dummy, NOT a zero-byte descriptor. BAD reads P as u64; raw280 has unknown extent→min(8MiB,deviceLimit); raw2A0 inferred4B→min(AlignUp(4+padding,alignment),deviceLimit). Both retain first-block clamp at these unsized descriptors. Counterfactual legacy_clamp is ONLY a view-size calculation on current runtime data/mapping, not a recovered historical GOOD backing/content. GOOD had no new deviceLimit clamp.

CachedMappedBufferView::Update passes the chosen mapping span into FindOrCreate/TryGetView. Its size becomes BufferView::size. BufferView::GetBinding follows delegate successors and adds their offsets while KEEPING that size in VkDescriptorBufferInfo.range. Underlying physical guest backing may remain much larger.

## Device protocol: same two APKs, no further build sequence

1. Use MotoG100/Android12/Turnip24.0.99, Zelda1.0.0, identical resolution/options. Install A, load the same save and park the camera at a reproducible location. Keep characters/objects/effects/shadows/interfaces visible. Do not compare menus/loading with gameplay.
2. Warm the game, then hold that same scene for90s. Save the INFO log and note overlayFPS; make a screen recording or comparable screenshots to verify full rendering. Input data equality cannot be inferred from node counts alone; this is a controlled device comparison, not deterministic command replay.
3. Fully stop the emulator/process before replacing with B. Keep settings/save/driver identical, warm similarly and repeat90s. Separate processes are REQUIRED: an8MiB backing created earlier can persist after a smaller view, invalidating a within-process switch.
4. Reuse A for a final A-B-A check at comparable temperature. This uses the SAME TWO APKs, no third build. If A-B-A drift or camera/workload differs materially, do not attribute the difference to range size.

## Logs and predeclared verdict

One certificate line at target compilation; five-second SsboRangeAB fps lines count completed PresentFrame calls per monotonic wall interval (present_hz), separately showing existing overlay_fps. Not GPU timestamps or guaranteed guest simulationFPS.

Rows are aggregated, not per draw. They carry legacy_word, shadow legacy_clamp, modern/selected request, prepared view, dispatch groups, actual deferred descriptor range_min/max, physical backing_max and prepared_backing_max. Actual descriptor resolution is observed under the ORIGINAL recreation mutex; no forced early resolution, resource locks, fences, memory reads for hashing buffer inputs or extra copies. fixed_binding includes megabuffer AND dummy variants; reject truncated rows before interpreting it. Extrema are not coherent tuples; do not combine independent maxima to invent an access interval. Cumulative prepared/resolved observations are not an atomic snapshot.

Validity requires certificate accepted, target calls in both arms, dropped=0, truncated=0, resolved range reduction really present in B, matching scene/groups/rendering and stable A-B-A. Zero legacy word or legacy_clamp<4608+padding means an exact recreation of GOOD would NOT guarantee all proven accesses; the experiment deliberately does not reproduce omission or dummy data.

- B recovers substantial FPS with unchanged rendering and repeatable A-B-A: this supports target range sizing as causal. The measured physical backing change tells whether coalescence amplification also changed. Still require a general compiler/host fix on latest master and Marvel gameplay validation, not shipping this hash guard.
- B has the intended view/range reduction but no FPS recovery: reject this TARGET's range size as a sufficient/dominant solution in this scene. Do not request another progressively smaller cap. The already-approved next direction is a different GPU cost, not more upload tuning.
- If backing remains large, that negative result does not independently refute full-buffer coalescence/dirty costs. Nor does it exclude the unmodified unknown290 or all possible SSBOs. A global verdict is not justified by a targeted experiment.
- Failed certificate, missing/truncated mapping, dropped rows, changed rendering or unmatched scene/temperature: invalid experiment; no cause or speed claim. Do not automatically create another build.

## Host verification

python3 tests/ssbo_range_ab/run.py

Tests extract the actual complete GetStorageBufferBinding body, run768 cases per arm, compare A and unaffected B paths with c7cb28d, exercise legacy0/small/large words and alignment4/256, ensure barriers/dirty/attachment calls unchanged, test certificate refusal, atomic attribution/overflow and actual deferred GetBinding with a coalesced delegate offset. UBSan. Test-only fakes represent Vulkan/GMMU; no claim of device concurrency/graphics validation. Policy RED asserted8MiB!=4608 before implementation; GREEN passed. Existing shader/compiler tests were not repeated.

The workflow builds both ARM64 DevRelease APKs and checks APK/libskyline.so/provenance. Success/artifacts remain unconfirmed until independently observed. Do not monitor Actions after submission.
