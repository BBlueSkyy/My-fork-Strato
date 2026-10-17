# GMMU stage 1

Base: master 66637da0. New branch wip/gmmu-stage1; separate Draft PR.
Goal: exact resident/sparse/unmapped translation through fragmented map/unmap/remap,
without fabricated physical pointers, stale block extents, or global GPU waits.

Keep FlatMemoryManager's O(1) segment lookup for resident mappings. Give the memory
manager its own interval replacement operation, leaving FlatAllocator unchanged.
Store sparse state separately from physical pointers. Expose typed translations;
legacy physical-span consumers receive null spans for nonresident bytes, never a
sentinel or an anonymous host page passed to process CreateMirrors. Preserve
zero sparse reads, discarded sparse writes and faults for unmapped CPU accesses.
Do not import #234's private texture mirrors: its loading freeze was unresolved.
Fully sparse/partially sparse Vulkan resources remain a separate integration
stage; this stage must describe that limitation, not claim games are fixed.

Refresh the fast index's surviving boundaries after partial updates. Validate
address bounds/overflow/alignment before mutation. Preserve callbacks for real
physical bytes only. Buffer-view mapping caches must detect remaps. Physically
contiguous fragments may share a physical view; genuinely disjoint GPU buffers
require a future multirange BufferManager and must not be presented as contiguous.
No NCA, kernel, allocator redesign, game hacks, extra copies or global submits.

Evidence: master multi-block translation exposes 0xCAFEBABE; cached head extents
cross remaps; SegmentTable L2 demotion has an inverted tail-count ratio. Eden/Yuzu
MemoryOperation distinguishes mapped/reserved/unmapped. Ryujinx MemoryManager
Translate guards PteUnmapped before offset arithmetic and uses explicit multiranges
plus cache invalidation events. #234 is analysis only; #244/#353 remain unchanged.
