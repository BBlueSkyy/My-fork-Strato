# PR454 evidence and specialization correction

User authorization: implement the supplied four-part correction request inline,
from d951ca35, preserving PR455 3087d5a5 as diagnostic reference. No merge.

## Design
- Replay captured Maxwell binaries with pinned compiler, captured shader header,
  base offset and compile-time metadata. Validate SPIR-V and prove descriptor-0x30
  samples are control-dependent on CBUF3[4], with zero/nonzero controls.
- Keep reserved/unmapped sampled binding compatibility on nullDescriptor hosts.
  No global zero-handle rewrite, storage discard or shader code change.
- Track exactly the CBUF words, TIC types, integer-format and sampler comparison
  values queried by compilation. Normalize/deduplicate this snapshot; revalidate
  CBUF handles before TIC lookups to avoid consulting obsolete selected slots.
- Store multiple owning pipeline variants per existing packed key; never replace
  a pipeline in use. Match current pipeline, transition hits, graphics manager,
  compute manager and preloaded disk bundles against the snapshot.
- Use PipelineState Refresh to cover draws where register dirty tracking skips
  Flush. Revalidate queried inputs; do not recompile for unchanged inputs or
  runtime-only P0 changes. Existing cache serialization layout remains v5.

Alternatives rejected: per-draw compile harms CPU/frametime; keying only on TIC
index misses changed CBUF handles; replacing unique_ptr invalidates live pointers;
whole-pool hashes/epochs invalidate unrelated variants and miss CPU memory writes.

## Tasks and executable gates
- [ ] Replay and inspect exact captured SPIR-V; no compiler fix without defect.
- [ ] Reproduce four original cache assertions before edits.
- [ ] Test normalized snapshots, handles-first matching and selective invalidation.
- [ ] Integrate snapshot capture and all cache/dirty/preload reuse paths.
- [ ] Run original cache probe unchanged in expected behavior, then expand with
      valid-invalid-valid, retained lifetime, preload and no-recompile controls.
- [ ] Run ASan/UBSan, real shader compiler, Vulkan validation and regressions.
- [ ] Independent review; publish only tested corrections to PR454, keep PR455.
- [ ] Request ARM64 Release only after mandatory gates pass; no merge.

Ledger: original 1,117 checks and all d951ca35 artifacts remain intact. Raw package
is preserved under ../kirby-evidence; no added runtime instrumentation is planned.
