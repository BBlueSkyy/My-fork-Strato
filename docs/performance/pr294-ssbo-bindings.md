# SSBO bindings — next isolation, not a correction

Base master b8593d8b2c2e9742a4f1f9edfaa935f653cd3663; compiler remains
c10731c2d5ab00dbc115e651edbb88e221dcf9cc. No #405/#406 changes.

Device evidence supplied by the user:
- Bad c7cb28d + old compiler/SSBO (9da6fda2, run 41161603593): Zelda fast,
  Marvel magenta returned.
- Good 28bcb957 + new compiler/SSBO (d4ef6121, run 41163909934): Zelda slow.

The group is isolated in both directions. Which internal change causes the cost
is NOT isolated. Full rollback is disqualified by Marvel. This build observes
real bindings without altering has_size, pointer recognition, 8MiB fallback,
compute inference, descriptor range, synchronization or texture semantics.

## Collection

Moto G100, Turnip 24.0.99, INFO logging, existing readback options OFF for both
runs. Use the same existing settings otherwise. Run Zelda in the slow scene for
60 seconds after loading, exit and export its log. Then run Marvel in the scene
where magenta previously appeared for 60 seconds, exit and export that log.
Confirm full objects/textures/shadows/effects/UI. One diagnostic APK serves both
runs; no second settings mode or build. Data may contain menu and loading work;
compare deltas over the actual gameplay portion.

## SsboDiag schema (cumulative; no per-draw logs)

Binding rows key the actual host stage mask, cbuf index/offset, metadata kind and
written flag. Stage flags include vertex 0x8, tess control 0x10, tess eval 0x20,
geometry 0x40, fragment 0x80, compute 0x800. kind 0=sized, 1=raw graphics,
2=raw compute with accessSize, 3=raw compute unknown. has_size is recorded from
that classification, never synthesized to control a binding. written is compiler
metadata, not a count of GPU stores actually executed.

An offset congruent to 8 modulo 16 is evidence of an 8-byte-aligned binding;
alignment alone does NOT identify the exact compiler tracking branch or prove
it caused corruption or cost. No guessed raw-pointer size field is read.

request_bytes/view_bytes sum requested range and returned BufferView::size over
binding calls. max_request/max_view/max_backing expose maximum requested range,
returned view size and underlying guest-backed Buffer span, respectively. Values
can overlap and repeatedly count the same bytes. They are NOT bytes processed
by a shader, copies or unique mapped pages. Reused views count as observations,
not allocations. misses counts post-Update empty views; early null/zero-range
bindings and exceptions before observation are outside these counters.

direct_calls counts bindings whose buffer uses direct import. Buffer information
is read only after the existing executor attachment has acquired its lock.

Totals: frames and elapsed_ns measure host presentation progress (not physical
scanout or guaranteed unique game frames). all_buffer_uploads/upload_bytes and
all_buffer_downloads/download_bytes count actual memcpy calls/bytes in normal
staged Buffer::SynchronizeHost/Guest. They include buffers other than SSBOs;
exclude direct import, GPU copies, fast-readback copies and other memcpy paths.
all_buffer_dirty_marks/dirty_span_bytes count actual MarkGpuDirty calls and full
guest spans inserted in usage tracking, repeatedly including overlapping spans.
Do not interpret them as unique dirty bytes, sync duration or bytes processed.

The fixed table has 128 slots, at most 16 probes per binding, no allocation or
new mutex. Dropped observations are explicit. At most 16 changed binding rows
plus one totals line are emitted every five seconds, plus the first frame;
reporting rotates across slots so a row may span several reporting windows.
Capture 60s; if dropped>0 or coverage remains inadequate, do not infer absence of
a path. All snapshots are relaxed and non-transactional. Reporting is after the
existing frame callback and vsync signal, outside PresentFrame's resource locks.
No file IO in descriptor binding. Instrumentation adds CPU work, so this APK is
not the final FPS benchmark baseline.

## Local validation and scope

Counter tests under ASan/UBSan cover kind/key encoding (including full U32 offset),
concurrent accumulation, maxima, missing views and explicit table overflow.
A host test extracts the entire production GetStorageBufferBinding and invokes
sized/raw graphics/compute and inferred ranges with facade resources under UBSan;
reported view size matches the returned binding. This is not an Android/GPU test.
Source checks preserve the original binding algorithm and all pre-existing
resource synchronization decisions. Compiler, CMake and Gradle are unchanged.
The existing workflow builds only ARM64 DevRelease from the exact pushed SHA
and emits APK SHA256/provenance. PR404 and previous experiments are untouched.

## Interpretation gate

First use the logs to see whether Zelda actually uses raw graphics legacy-aligned
positions, 8-mod-16 positions, inferred compute or unknown compute. Compare view
and backing expansion, then normal staged copy/dirty volume per host frame;
these global totals cannot alone attribute a cost to an individual binding.
If a binding is implicated, inspect its real shader address expression/range
before a smaller compatible experiment. Unknown bounds stay unknown; never cap
8MiB arbitrarily or mark all old 16-byte candidates sized. Marvel's observed
bindings and graphics must remain correct in each subsequent candidate.

No narrower behavior-changing APK or permanent Draft fix is published until
these paths/ranges are evidenced. Actual shader bounds, per-resource sync-time
attribution, recovery of Zelda FPS and preservation of Marvel graphics remain
unvalidated. A permanent fix must target latest master after that validation.
