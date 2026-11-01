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
