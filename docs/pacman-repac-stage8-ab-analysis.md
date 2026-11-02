# PAC-MAN WORLD Re-PAC — Stage 8 HDR/post A/B comparison

Date: 2026-11-01
Repository: BBlueSkyy/My-fork-Strato
Draft PR: #414
Device: Moto G100, Adreno 650
Game: PAC-MAN WORLD Re-PAC (Switch)
Threshold: 256 in both runs.

## Source files

User provided:
- `capture_001 A — Com submissões.zip` and `A — Com submissões.log`
- `capture_001 B — Sem submissões extras.zip` and `B — Sem submissões extras.log`

The user independently confirmed **A displays no inverted triangle** and
**B displays the triangle**.

No screenshot of GPU readback content is contained in either ZIP;
the visual result is user-observed, while all metadata below are
directly confirmed from `capture_complete.txt` and `gpu_events.txt`.

## Verified counters

| Metric | A — working | B — broken |
|---|---:|---:|
| Diagnostic mode | 7 | 12 |
| Host presentations traced | 60 | 60 |
| Postprocess candidates | 236 | 236 |
| Additional executor `Submit()` invocations | 236 | 0 |
| Events recorded | 532 | 296 |
| Dropped events | 0 | 0 |
| Experiment remains active after logging | true | true |
| Candidates already triggering natural threshold flush | 0 / 236 | 0 / 236 |
| Candidates with `gotoNext` forbidding submit | 0 / 236 | 0 / 236 |

Thus the two runs exercise an equivalent observed set of 236 postprocess
candidates and differ specifically in *whether the diagnostic requests
additional real submissions* at those points. The host present interval
numbers are not identical for 31 candidates, as queue timing changes
with submissions; they are not exact guest frame numbers.

### Exact recurring four-candidate sequence

Both captures contain **59 complete groups of four**; by candidate
ordinal, the shader metadata, target dimensions, format IDs and sampled
input dimensions/format IDs match for all 236 corresponding candidates.

| Candidate slot | Observed output | Observed sampled input | Fragment shader hash | Same exact HDR producer texture? |
|---|---|---|---|---|
| 1 | 320×180, format 122 | 1280×720, format 122 | `907BB0C918B4A53A` | yes |
| 2 | 1280×720, format 37 | none in `sampledImages` | no associated Maxwell draw signature | no |
| 3 | 1280×720, format 70 | 1280×720, format 129 | `E023BE6EDCD1F173` | no |
| 4 | 640×360, format 122 | 1280×720, format 122 | `6D46508ECDC8D0FA` | yes |

All recorded postprocess input-attachment counts were zero. Every
candidate used `natural_flush=false` and `goto_next=false`.
Sampled image count is one in slots 1/3/4, zero in slot 2. Identity
matching proves the *same Texture object* for slots 1/4, but cannot
exclude overlapping guest-memory aliases for other inputs.

The shaders with available Maxwell signatures used the same vertex
hash `0B107DCDE08A7AAA`, an indexed six-vertex draw and an output
scissor matching the target dimensions.

The `sub` field directly reflects extra submit behavior:
- A: across each four-candidate group, the submission numbers advance
  sequentially (offsets `0,1,2,3`).
- B: candidates 1/2 and 3/4 share respective submission numbers
  (offsets `0,0,1,1`); normal submits still occur elsewhere.

**Interpretation:** this sharply localizes a submission-sensitive
interaction at specific HDR/postprocessing workloads. It is not yet
proof of a missing pipeline barrier, stale texture backing, queue-order
bug, cache-coherence fault, or Vulkan driver quirk. A real `Submit()`
also invokes flush callbacks, `SubmitInternal()` texture/buffer
synchronization, `WaitSubmit()`/cycle chaining, tracking resets,
slot rotation and host-driver scheduling. Renderpass-only split and
split+generic barrier were separately tried without correcting the
artifact.

## What to test next: isolate which one of the four candidates matters

Already implemented in this same Draft PR, **no extra source change is
needed** for these four controlled tests:

- `ENABLE_EXECUTOR_POST_SLOT_1_TEST` (mode 8)
- `ENABLE_EXECUTOR_POST_SLOT_2_TEST` (mode 9)
- `ENABLE_EXECUTOR_POST_SLOT_3_TEST` (mode 10)
- `ENABLE_EXECUTOR_POST_SLOT_4_TEST` (mode 11)

For each: set threshold=256, quit Strato completely, leave **only**
the selected flag plus a fresh `ARM_CAPTURE` under
`gpu_capture/pacman_world/`, boot to the same scene, observe the
triangle while the persistent mode is active, and archive ZIP/log.
Each individual filtered test should select approximately **59**
additional submits in the first 60 host presentations, versus 236
for mode 7, assuming the same event sequence. Check reported mode,
nonzero forced-submission count and stable candidate signatures.
Report whether the triangle is corrected and approximate FPS.

The initial two slot tests of greatest diagnostic interest are **1**
(directly samples the exact HDR source at 320×180) and **4** (samples
the same HDR source at 640×360). Test slots 2 and 3 too if needed;
the current evidence does not establish which candidate is causal,
and slot 2 has no associated Maxwell draw signature, not proof that
there was no GPU operation.

If one slot uniquely corrects rendering, compare its exact
producer/consumer texture, cycle, transitions, and callback operations
in `SubmitInternal()`. Test a structural resource/ordering correction
that removes the need for an extra submission, keeps default
`Executor Flush Threshold=256`, and does not regress performance
or other games. No hardcoded dimension, format or shader hash should
be part of the final fix.

Validation targets eventually include PAC-MAN WORLD Re-PAC and
reported visually similar Unity games Marsupilami: Hoobadventure and
Prince of Persia: The Lost Crown. The latter two have **not yet**
established the same causal mechanism.

No merge; PR remains Draft. No definitive graphics fix has yet been
validated.
