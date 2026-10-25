# Physical region authority implementation plan

Continue directly in Draft #400, preserving baaf3cd2 and its eight ancestors. Execute inline under the user's explicit implementation instruction.

Goal: real Vulkan A→B→reused A has 0/1024 stale texels and preserves exclusive bytes. Native 2D compatible dense block-linear layouts first; multiple physical mappings, mips/layers where row correspondence is provable. Preserve unsupported existing paths by draining authority before leaving this domain.

Architecture: fixed physical interval partitions built at overlap registration, with separate host-writer and guest-publication generations. Each member holds only its interval bindings and neighbor invalidations. A stale host is refreshed by asynchronous, submission-ordered GPU row copies; readback encodes to scratch and publishes only intervals still owned by its source. Disable guest eviction while shared regions need the guest bridge. No bitmap footprint construction or global GPU completion wait.

Alternatives: whole-texture ordered readbacks cannot preserve independent interval writers. A general layout overlap graph is outside this stage and repeats #393's risky scope. The selected row proof admits compact direct Vulkan copies without per-texel metadata.

1. Convert the existing real Vulkan probe into a failing assertion (including exclusive regions), run RED on unchanged production.
2. Implement immutable physical partitions and writer/guest versions; test fragmented mappings, transitive disjoint regions and alternating writers. Common no-overlap path does not allocate or scan the manager.
3. Integrate initialization, cached attachment/reuse, submission dependencies, masked guest publication, CPU traps and failure rollback. Retain views/backings. Drain complete components before incompatible layout transitions. Test pending producers, CPU writes/reads and FreeGuest ON/OFF.
4. Audit retained descriptors and storage preflight so no failed retry publishes a phantom writer. Add cheap generation invalidation rather than global texture scans.
5. Run required Vulkan oracle and essential existing suites, review independently, document measured evidence and limits, publish minimum commits to same Draft. Workflow/build configuration unchanged; no device FPS/UE4 success claim, no Actions monitoring or merge.

Checkpoint status: data-path oracle and essential regression suites are green, including ASan/UBSan for the authority target. Publication gate remains RED at `TEXMAN_LOCK_ORDER_PROBE=1`: retaining a component mutex through general preparation inverts presentation's texture/component order. Current local implementation is WIP and must not be pushed. See `docs/region-authority-checkpoint.md` for the counterexample, test coverage, corrected donor revalidation and remaining acceptance work. The nine existing PR commits are preserved.
