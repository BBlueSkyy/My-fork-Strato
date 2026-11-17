# PR #362 final consolidation audit — 2026-11-16

Checkpoint: `75dfb2ea02f99d68d85f6d92ad7eb73e533a67da`.
Master: `f98c9bb6b80a2bc145e07058176b82d9917ae213`.

## Scope and device validation

The user reports at least seven consecutive successful MK11 boots, gameplay,
normal controls/audio, and approximately 50 other games without perceived
regressions. These observations support consolidation; they are not an
instrumented performance study or proof for every Android/driver interleaving.

The cleanup removes only temporary queue/register capture, its dedicated
signal/handlers, mailbox, compile options, capture-only tests/runner and
activation document. It preserves the functional idle-core migration request,
HostSignalHandler acknowledgement, TrySendSignal, parked wake state, scheduler
fixes, adaptive-condition-variable fix and swapchain lifetime protection.

The four runtime scheduler/KThread files are exactly equal to checkpoint source
with disabled capture blocks removed (plus the now-unused cerrno include).
The swapchain, functional SVC and condition-variable code are unchanged.
CMake/Gradle no longer differ from master for the removed capture options.

## Audit and validation

Independent review found no concrete blocker in migration requests, signal
handoff, guest-return boundaries, priority/affinity enforcement, park/wake lock
ordering or swapchain retirement. No functional code change was warranted.

Executed after cleanup:

- Scheduler: 36 cases under ASan/UBSan and TSan; 50 complete UBSan repetitions
  (1,800 cases). The independent reviewer also ran all 36 UBSan cases and the
  failing pre-fix idle-front negative control.
- Swapchain presentation: ASan/UBSan, TSan and 25 UBSan repetitions, 31 assertions
  per repetition (775 repeated assertions).
- GPU submission, sampled layouts/aliases, null storage image, queries,
  DRAW_TEXTURE and NCE acquire-lock/wideLines regressions passed.
- Functional-source equivalence, absent runtime/build capture identifiers,
  Python syntax, workflow YAML and git whitespace checks passed.

The first null-storage run hit this host's known LeakSanitizer/ptrace limitation;
it passed with ASan/UBSan active and detect_leaks=0, as used for the other gates.
There was no new product-code failure to correct. Earlier unrelated conditional-
rendering test-facade/Boost limitations remain documented in the prior audit.

The final ARM64 DevRelease build is required before merge. Its workflow verifies
ZIP CRCs, manifest/classes, AArch64 ELF headers for every native library, and the
APK signature using Android build-tools apksigner. Actual build/run/artifact
results are recorded in the PR description before merge. Gradle/CCache and all
existing GPU/compiler gates remain intact.

All 36 functional scheduler cases remain; only the four capture cases
and mailbox tests are deleted. Existing backoff, priorities, affinities and
signal paths are not retuned. No new optimization or subsystem change is added.

The permanent [scheduler investigation](mk11-scheduler-yield-progress.md),
[material-worker evidence](mk11-material-worker-stall-20261116.md), and
[swapchain audit](mk11-pr362-20261116-audit.md) retain root causes and limitations.

## Remaining limits

Host tests use boundary facades; Android execution is covered by the user's
device testing and ARM64 compilation, not by host TSan. LeakSanitizer is disabled
because this host cannot inspect process tasks; ASan remains enabled.
The swapchain drain covers submitted GPU-copy lifetime at surface/extent/format
replacement, not ordinary frames. It does not claim formal WSI presentation-
engine completion without the maintenance extension (see the earlier audit).
There is no new evidence assigning the older native SIGSEGV to a particular
resource or thread. Historical TSC/audio warnings are not treated as causes.
