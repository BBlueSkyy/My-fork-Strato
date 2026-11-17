# Master integration checkpoint — 2026-11-09

Initial remote PR400: `dfa18b4348fe39b950761acd448792552846a647`.
Initial remote master: `f0a95dd80eb150e81d16e1b0fa88c3a812a820c8`.
The original dirty workspace is preserved at
`refs/checkpoints/stage-c-pre-master-20261109` (`5e3bd9f6`). The domain-cache
checkpoint `897528ab` and earlier local refs were recovered. No functional guest
bridge was present: only its pending sRGB content test and design were recovered.

This merge incorporates master, including #428 and #430, without rewriting
published PR400 history. Shader compiler stays at master's exact
`a987e2a5c8fa507c805d5867737de5976c7c2ce0`.

Five textual conflicts were resolved incrementally: common pipeline binding,
textures implementation/header, texture includes, and sampled-layout extraction.
Null descriptor decisions stay before attachment/writer publication; valid storage
writers retain PR400's deferred commit. The TIC cache is keyed by shader type
and retains B1a mapping receipts, invalid-view lifetimes, and executor leases.
Pool remapping and minimum layered-backing fallback use tracked translations.

A semantic integration defect was reproduced in real Vulkan: two backing layers
behind a one-layer view omitted authority for the hidden layer. The independent
byte oracle found 4,096 hidden-layer and 8,192 guest mismatches. Physical spans,
materialization sizing and mixed admission now use the full backing layer count.
Both format-list variants report zero hidden/guest mismatches after the fix.
View exposure remains separate; unsupported base-layer/layout cases keep their
existing safe eligibility gates.

Validation on this integrated tree (Clang18, host lavapipe, no device claim):

- ASan+UBSan and Vulkan synchronization validation: full backing-promotion default,
  authority and lock-order gates; four original A→B→A cases remain 0/1,024 stale
  texels with Free Guest OFF/ON. Layered regression also checks all guest bytes.
- ASan+UBSan: sampled layout/oracle+alias suite; storage preflight, null storage
  descriptor policies and deferred-writer/allocation/retry paths; SSBO's 96 actual
  binding paths plus automatic epoch P→Q→P and equal-revision translator changes.
- Actual Vulkan mapping/cache gate: typed view cache, remaps, retained resources,
  RT/pool identities and publication retry, Free Guest OFF/ON.
- Mapping identity, production GMMU map/unmap and partial-generation gates; executor
  submission lifetime gate; both subgroup source contract checks.

The local Vulkan ICD initially lacked LLVM20. Restoring its runtime dependency
was required before counting content results. One compilation encountered an
unreadable host header; the unchanged source compiled and passed on retry. These
were environment failures, not data-correctness results.

MixedRegionAuthorityEnabled remains false. This integration does not implement
or claim the persistent guest bridge, safe DMI/nonreversible arrival protocol,
phone FPS, Android driver validation, or any game-specific repair. Odyssey's
successful gameplay after dfa18b43 remains the user's device baseline. Gravity
Circuit's earlier shader failure and PR421 remain separate and untouched.
