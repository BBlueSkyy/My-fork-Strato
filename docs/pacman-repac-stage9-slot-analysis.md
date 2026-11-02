# PAC-MAN WORLD Re-PAC — Stage 9 individual slot A/B

## Evidence from Nov 2, 2026 device runs

The user submitted four distinct mode captures (ZIP + emulator log) with
default `Executor Flush Threshold=256`, plus a screenshot documenting
**partial** recovery in the mode-10 SLOT 3 test.

| Test | Mode | Host presents | POST_CANDIDATE events | Forced submits | Dropped events | User's visual result |
|---|---:|---:|---:|---:|---:|---|
| SLOT 1 only | 8 | 60 | 244 (61×4) | 61 | 0 | Large triangle remains |
| SLOT 2 only | 9 | 60 | 236 (59×4) | 59 | 0 | Large triangle remains |
| **SLOT 3 only** | **10** | **60** | **236 (59×4)** | **59** | **0** | **Major visual improvement, but a faint inverted triangle still remains** |
| SLOT 4 only | 11 | 60 | 236 (59×4) | 59 | 0 | Large triangle remains |

All four captures confirm `executor_experiment_persists_after_capture=1`.
Logs explicitly confirm `executor_test=8/9/10/11` and completion.
All individual candidates report `natural_flush=false` and
`goto_next=false`: these are genuinely additional `Submit()`
calls, not merely re-counted normal submissions.

The first run had 61 four-slot groups, while the others had 59.
Therefore, **host presentations are not identical guest frame
boundaries**, and the modulo-four slot index is not a general
semantic phase identifier. Nonetheless, the four-pass order/metadata
is stable in these captures.

## Candidate identity

Every one of the four tests saw the same repeated pass sequence:

1. SLOT 1: HDR 1280×720 format 122 sampled as an image, output
   320×180 format 122, FS `907BB0C918B4A53A`.
   `sampled_same_hdr=true`.
2. SLOT 2: 1280×720 output format 37, no sampled images enumerated.
   No associated Maxwell draw signature.
3. **SLOT 3: sampled source 1280×720 format 129, output 1280×720
   format 70, FS `E023BE6EDCD1F173`.**
   `sampled_same_hdr=false` (only excludes exact same texture object,
   not an alias or broader data dependency).
4. SLOT 4: HDR 1280×720 format 122 sampled, output 640×360 format
   122, FS `6D46508ECDC8D0FA`.
   `sampled_same_hdr=true`.

**Interpretation:** a real submission immediately following the SLOT 3
draw is necessary for most of the visual improvement in these isolated
tests, but it alone is *insufficient* to eliminate the faint residual
triangle. The A/B does not establish that SLOT 3 is the only possible
remediation or prove a particular `SubmitInternal()` sub-operation is
the root cause.

## Stage 9 follow-up: paired slot tests

### Paired results now verified (2026-11-02)

The supplied pair 1+3 run recorded mode 13, 244 candidates, 122 extra
submissions, 60 presents and zero dropped events; the user still observed
the residual triangle. Pair 3+4 recorded mode 15, 236 candidates, 118 extra
submissions, 60 presents and zero dropped events; the user observed correct
graphics. Both diagnostic modes persisted after capture completion, and
neither run had a candidate with a natural threshold flush or `goto_next`.

Thus 3+4 is sufficient in this execution. This does not prove that the
two passes directly depend on each other, that the HDR object is corrupt,
or that a particular barrier is missing. The submission-contract audit
and the one new callback-only discriminator are documented in
[Stage 10](pacman-repac-stage10-submit-contract.md).

The following subsection records the original Stage 9 test procedure.

The same Draft PR #414 adds three opt-in markers, all with
`Executor Flush Threshold=256`, re-armed `ARM_CAPTURE`, and a
complete Strato process restart for each independent experiment:

| Marker | Mode | Forced submission slots | Approx. per 60 host presents |
|---|---:|---|---:|
| `ENABLE_EXECUTOR_POST_SLOTS_1_3_TEST` | 13 | 1 + 3 | ~118 |
| `ENABLE_EXECUTOR_POST_SLOTS_2_3_TEST` | 14 | 2 + 3 | ~118 |
| `ENABLE_EXECUTOR_POST_SLOTS_3_4_TEST` | 15 | 3 + 4 | ~118 |

**Test 1+3 and 3+4 first**, because slots 1/4 consume the exact HDR
producer Texture object; test 2+3 if both still exhibit a triangle.
These are **diagnostic grouping tests**; a positive or negative result
does not directly identify an incorrectly implemented Vulkan barrier
or resource ownership transition. Multiple additional submissions can
alter CPU/GPU scheduling and callbacks independently.

No runtime intervention occurs unless an explicit experiment file and
`ARM_CAPTURE` are used. Experiments persist beyond their bounded
60-host-present log window until the process exits. The broad
`ENABLE_EXECUTOR_POST_TRACE` (mode 7, known to eliminate the
triangle) and no-extra-submit `ENABLE_EXECUTOR_POST_CONTROL`
(mode 12, known to show it) are unchanged.

Do **not** treat the SLOT 3 test as a full graphical fix. Do **not**
apply a global flush threshold 0, disable barriers/traps, or hardcode
this game's dimensions, formats, or shader hashes as a production
patch. Once a stable minimum correcting subset is identified, compare
resource producer/consumer lineage, cycles, renderpass state, flush
callbacks, submission ordering, and slot-rotation behavior against the
known-good and known-bad traces. Only then request a tested structural
fix in Work. Keep PR Draft and unmerged; no ARM64 device/build
validation of new paired modes yet.
