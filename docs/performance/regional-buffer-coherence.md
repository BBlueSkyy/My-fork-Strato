# Regional buffer coherence: staged, independent implementation

Base master fc8eb5196c198b281dce6bb3d9a9126b11ac5d4a, SSBO#409/compiler unchanged.
PR400/415 are references, not merged. The supplied v2/v3 logs show buffer readback
preparation dominates memcpy and whole-buffer dependencies; producer-only PR415
has no reported perceptible Zelda benefit. No device FPS gain is claimed.

References consulted: Yuzu Project Y.F.C.1.90 April2023 report (PR10084/10088
original links unavailable); Eden official mirror buffer_cache.h blob f30c85911f525c9d50a100a49abeb6da81e7df37,
buffer_cache_base.h26bf3dd9ed04983a21e3d611890b00e5d595341e,
memory_tracker_base.h b8547f3149690652c47619cf4bf18bd0d7bbdc9b,
word_manager.h76d608bbe0dc375a496201daae4796aa4cd8e71c.
These source files declare GPL-3.0-or-later. No source text, data structures or
implementation is copied. New Strato code is independently implemented under
MPL-2.0. Existing files keep their original licenses, including GPL TrapManager.

Comparison:
- Eden tracks CPU/GPU modified ranges separately and downloads intersections.
- Its async path stages copies, fences them, then publishes only still-valid
  ranges while retaining staging lifetime. This requires generation ownership.
- YFC report also describes relaxed Normal-accuracy fence policies; do not port
  them as correctness-preserving improvements.
- Strato staged backing is already host mapped; mandatory demand download is a
  host memcpy after a fence, not the same staging architecture. Moving that copy
  to another thread alone does not remove the producing command dependency.
- Strato one trap/DirtyState per backing makes unrelated CPU accesses wait.

Chosen first functional stage: page-aligned authority partitions, each Clean,
CpuDirty or GpuDirty, with monotonic boundaries introduced by known write ranges.
Use a trap per partition, not per page; common large ranges stay a single trap.
Only a faulting partition needs download/CPU ownership. Preserve GPU-dirty siblings
while uploading CPU-dirty partitions. The coarse DirtyState remains a conservative
summary. Unknown writers and coalesced pending writers mark the full backing.

All bound declarations are conservative potential writes, not actual shader stores.
Use GetOffset after delegate retargeting. Every writer path, including SSBO,
storage texel/image buffers, TF, queries, DMA and inline writes, must be covered.
Direct imports retain their existing path. Rebuild traps by installing/protecting
all replacements before deleting old handles; no unprotected GPU-dirty window.
Callbacks capture scalar ranges and weak owners, never vector references.

Plan/test gates:
1. RED production callbacks: GPU writer in region A, CPU partial store in disjoint
   B; baseline waits A, candidate must progress while A remains pending/protected.
2. Implement a standalone, bounds-checked partition authority map and mixed state;
   check page boundaries, split permanence, unknown ranges and failure retention.
3. Integrate range traps, guest/host sync, programmatic writes/copies, coalescence
   and all GPU binding writers. No optional producer-only PR415 fields/hacks.
4. ASan/UBSan, real mapped aliases and traps, mixed CPU/GPU readbacks, full/partial
   writes, writer replacement, recreation/failure recovery and real Vulkan copy
   byte comparisons/sync validation. Review before first functional commit.
5. Commit/push immediately and open dedicated Draft. Existing ARM64 DevRelease,
   no diagnostic APKs, no Actions polling, no merge. Preserve every other branch.

Async/per-region producer cycles are separate future stages only if safe measured
benefit is demonstrated. This stage removes waits for regions with no GPU writer;
it retains full fence waits whenever requested data actually needs the GPU.

## Functional regional stage (PR416)

The staged path now keeps separate page-aligned CPU/GPU authority partitions.
`BufferRegionState` owns monotonic partition boundaries and the union of accesses
recorded for a submission. `Buffer` installs one trap per partition, downloads only
GPU-owned intersections and uploads CPU-owned intersections without overwriting
GPU-owned siblings. SSBO/texel/image bindings, vertex/index/indirect buffers,
transform feedback, queries, DMA and inline callbacks pass conservative view bounds
using the current delegate offset. SSBO range inference and the compiler gitlink
are unchanged. Direct imports keep their original synchronization path.

`SynchronizeHostForGpu` consumes the submission's access union, so deferring a CPU
write outside GPU region A does not merely move its wait to an unrelated A-only
submission. An unknown access falls back to the full backing. Backing mutation,
coalescence and actual GPU-owned CPU demands still retain the existing full cycle
and its predecessor chain. This does not relax FenceCycle or Vulkan barriers.

Coalescence publishes CPU-owned siblings before copying a whole source backing,
including a CPU-dirty source made immutable by a read-only DMA use. Pending
retargeted views conservatively register full backing use. Replacement traps are
protected before old handles are removed; allocation failure retains the old
scalar callbacks and their stronger protections. Empty synchronization ranges do
not change authority or page protection. Existing optional hacks preserve CPU-owned
siblings, but are disabled in the safe-path tests and are not part of the gain.

## Reproduced costs and completed checks

The existing negative-control fixture on master failed because a CPU store in B
blocked on a gated GPU producer in A. The regional implementation completes that
store and an A-only submit while the producer remains pending: **one unnecessary
producer wait becomes zero for this case**. A later demand for A still waits the
producer once. These are controlled dependency counts, not an FPS benchmark.
The supplied final logs contain no CPU fault offsets, so they cannot establish
how frequently Zelda will hit this newly separable case.

Commands run locally:

```
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 python3 tests/buffer_regions/run.py
ASAN_OPTIONS=detect_leaks=0 python3 tests/buffer_regions/run_map.py
ASAN_OPTIONS=detect_leaks=0 SANITIZE=address,undefined python3 tests/gpu_submission/run.py
ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_conditional/run.py
python3 tests/ssbo_bounded/run.py --compiler <shader-compiler-checkout>
VULKAN=1 ASAN_OPTIONS=detect_leaks=0:handle_segv=0 python3 tests/buffer_regions/run.py
```

The first harness extracts production Buffer synchronization/trap/read/write/copy
bodies, complete CoalesceBuffers and the actual master NCE fault-routing expression.
Android allocation/resource interfaces are host facades; guest/mirror mappings
share a real memfd and protection uses real mprotect/SIGSEGV. Checks include:

- Every-byte mixed downloads/uploads; partial same-page writes; replaced/chained
  writers; mandatory and disjoint faults; zero-byte demands; inline callback
  execution once; same-backing disjoint DMA; source authority during coalescence.
- Overlapping external traps retain their callback and the GPU page's protection.
- Concurrent CPU faults over 25 repetitions; wait failure/retry; invalidation.
- Trap replacement recovery across **19 injected allocation failures**, plus the
  foundation's nine interval-registration failures; no orphan callback after removal.
- **2,000** randomized page-authority comparisons, bounds, partial last page and
  access-union coalescing; **96** existing SSBO binding/mapping cases.
- Production FenceCycle/SubmitInternal host regression retains submission ordering,
  cancellation, chained lifetime and clean/direct dependencies; conditional rendering
  regression still passes. Its resource facade forwards into regional host sync.

Native Vulkan used llvmpipe (LLVM20.1.2), Khronos validation with synchronization
validation, ASan and UBSan. A real gated GPU fill/copy of A remains pending while a
CPU alias writes B and submits only A. The later required fence/readback and B
upload are retained; the next full Vulkan copy matches all 8,192 expected bytes,
with **zero synchronization-validation errors**. Allocation interposition tests run
in a separate host process so they do not replace LLVM's shared-library allocators.
LeakSanitizer is disabled because this environment cannot inspect processes;
AddressSanitizer and UndefinedBehaviorSanitizer remain enabled. The intentional
page-fault harness uses its own SIGSEGV handler (`handle_segv=0`).

## Limits and device validation

No Android device, Adreno/Turnip validation or FPS improvement is claimed. No new
diagnostic APK or product logging was added. ARM64 DevRelease remains the existing
PR workflow; Actions is neither queried nor awaited. Local native tests are not
a full Android compilation.

Page ownership is conservative: bytes sharing a page remain coupled, unknown
bindings and coalesced pending views can widen dependencies, and overlapping
legacy trap groups can conservatively widen fault handling. Partition/trap
counts and mprotect overhead can increase when a large backing is fragmented.
The full producer/lifetime chain is still mandatory when the requested region is
GPU-owned. Asynchronous memcpy alone would not remove that dependency; async
staging/generation publication and finer producer cycles remain deferred until
there is a proven safe benefit. No Yuzu Normal-accuracy wait relaxation is used.

On Moto G100, test Zelda Link's Awakening1.0.0 with Turnip24.0.99, then Marvel
Cosmic Invasion, SpongeBob Cosmic Shake and Dragon Ball Sparking ZERO. Initially
set Fast GPU Readback, Fast Readback Writes, Force Maximum GPU Clocks and Free
Guest Texture Memory OFF. Compare identical scenes/saves against master; record
FPS, real simulation speed, pacing, flicker/magenta/black textures and stability
separately. No 60FPS cheat. Other Drafts400/405/406/411/412/415 stay independent.
