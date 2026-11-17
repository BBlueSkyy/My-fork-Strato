# Conservative texture cache collection

Base: PR #462, 4396c0de. Diagnostic findings: PR #461, ddb7b042.

This implementation frees actual host images and allocations, not just staging.
The first recoverable class is intentionally narrow: one contiguous physical CPU
span, no GMMU mapping in any registered address space, CPU-authoritative bytes,
no alias group, no overlapping representation, no unpreserved GPU write serial, no guest-memory pin, no external
view/descriptor/executor owner and a completed fence. No guest pages are freed.

GMMU Map/Unmap record old physical spans and update interval coverage. Another VA,
another AS or any still-resident portion prevents collection. Reusing a VA updates
coverage before a candidate is inspected. All ASes created by AsGpu share the GPU's
tracker. Sparse holes do not count as physical memory. Index allocation failure
disables collection rather than authorizing release.

The tracker mutex serializes mapping mutations against CPU batch construction.
Lock order is global channel mutex, tracker mutex, then existing GMMU block locks.
Mapping callbacks perform no Vulkan work, trap cleanup or texture destruction.
At a changed mapping generation, the existing submission/flush path invalidates
TIC fast entries and the historical strong store before another batch can reuse
them. A manager-owned deferred queue retains raw view objects until both their previous
image-use fence and any current recorded submission complete. No late dependency
is appended to a submitted fence, and empty cycles are never retained.

Old physical ranges select candidates using the overlap index. A queue processes
at most 16 candidates per batch; an empty queue with no events returns immediately.
There is no full texture scan in a draw or frame. The bounded journal requests a
single event-driven full candidate selection only if 4096 changed ranges overflow.
Erasure publishes a freshly rebuilt index and survivor vector together. Allocation
failure leaves the original lookup/index and image ownership intact.

External users and unfinished fences defer collection. A reference count is only
a final ownership gate after mapping and data proofs; it never proves allocation
death. Trap weak promotions are serialized by the existing texture/state locks.
The CPU-authoritative state prevents any final destructor from encoding obsolete
image contents into reused guest bytes. #462 supplies exactly-once trap cleanup
and framebuffer retirement. Collection adds no GPU waits or readbacks.

## Preserved cases and limits

GPU-dirty images, unmatched write/readback serials (including skipped readbacks), aliases (including MK8 HDR/mip relationships), overlapping
representations, multiple physical mappings, freed guest mirrors and pinned
resources remain resident. Their authoritative bytes cannot be abandoned based
on a VA unmap, age, replaced flag or memory target.

This is cache eviction of reconstructible images, not a guest allocation GC.
A kernel allocation discard/reuse event is not inferred from nvmap handle release
or engine arena unload. Recycling suballocations inside an unchanged live guest
heap cannot be detected and remains blocked. The full physical allocation
generation protocol for GPU writers is still absent; it is not bypassed.

Thus MK11 benefits only if arena changes unmap eligible GMMU spans. The existing
captures do not establish that event pattern; device RAM/FPS improvement requires
a Moto G100 A/B run. VMA reserved blocks and RSS need not shrink with every
destroyed image. PRs #441/#457/#461/#462 and scheduler #362 are not modified.
