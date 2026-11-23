# PR #393: experimental refresh on current master

Original branch: `fix/texture-overlap-authority`.
Original head: `91ceff3bbe31a66056da814255e2f0ef61107ed0`.
Integrated master: `e8e38237aa5993deeaaae9f01f39ea08500f18db`.

Preserve both histories with a merge commit, retarget the existing Draft to master and request the existing ARM64 DevRelease workflow. No merge into master, new PR or imported #353/#400 architecture. The user reported no significant visual improvement with #353; that is motivation for testing this independent existing design, not evidence that #393 fixes either game.

## Existing mechanisms and integration

| Mechanism | Already in current master | Refreshed #393 |
| --- | --- | --- |
| Ordered mappings and nested/secondary overlaps | #385 physical-range identity, logical offsets and batched overlap index | Retain these helpers/index; collect dependencies before cached full-match return, including retained/replaced live views |
| Cached descriptors / quick bind / clean RTs | #484 already checks alias coherence before descriptor preparation | Reuse the same early guards for both exact and regional stale state; no late throwing render-pass guard |
| Queued writer readback | #484 explicit pendingGpuWriteTag, sorted resource locks and submit/retry | Retain the exact-only fallback; coexist with the original regional registration/reconciliation request |
| Readback validity | Exact AliasGroup publication is already guarded | Retain #392/#393's successful publication for standalone and regional data, including the Linear copy-direction correction |
| Persistent regional authority | Not present in master | Preserve OverlapGroup interval owner/serial/guest-valid ledger after images become Clean; mask publication and restore exclusive GPU-only bytes |
| Free Guest | Existing ordinary texture policy and sparse handling | Preserve that policy outside components; retain #393's no-discard policy for shared component pages/padding and zero-serial recovery of previously evicted sampled images |

`TextureStorage`, `TextureMappingCache` and the V3 UsageTracker are not present. No other experimental coherence framework is added. The original `OverlapGroup`, `OverlapState`, masked private encoding buffer, alias registration and prospective mip-footprint reconciliation remain the architecture under test.

The old branch contained HDR CBUF/descriptor/cubemap capture additions after `7c63c2e9`. Remove those additions from the final source tree, retaining their commits in history. Executor and Maxwell3D were reconciled using the functional #393 tree at `7c63c2e9` and current master. No new logging/capture/validation code is added to gameplay.

## Proven refresh regression

Current master distinguishes viewed layers from full backing layers. The old #393 encoder and footprint bitmap used `GuestTexture::layerCount`, which is now the view count for explicit backing descriptions.

A regression uses a two-layer backing with a one-layer view, overlaps another texture with its second layer and verifies every composite/exclusive byte. With the old layerCount encoder, the real Vulkan test fails its byte assertion. Use `GetBackingLayerCount()` for both encoding and footprint allocation; the same test then passes. The existing legacy two-layer description also passes. This is adaptation of the existing encoding model, not a new copy or authority feature.

Other changes are conflict resolution, removal of diagnostic additions, and host fixture/runner updates for current production APIs. No speculative graphical fixes or performance changes.

## Traps, lifetime and memory review

- TrapManager, IntervalMap, kernel memory/NCE mirror implementation, BufferManager and all submodule SHAs remain byte-for-byte current master.
- Retain current per-page reprotection/deletion and optimized overlap scans. Regional callbacks still try-lock under the trap mutex; blocking preparation stays outside it.
- Keep #393 rearmable RemoveTrap handles within regional components, strong source/target references during reconciliation and weak component membership. Never publish superseded bytes, including temporarily.
- Retain master WaitSubmit ordering, predecessor-cycle chaining and clearing of pending-write receipts at submission. Request layout uses an explicit overlapReadbacks assignment to avoid confusing the regional-registration boolean with the newer vector field.
- Preserve current sparse GMMU reconstruction, mirror reads, residency generation/traps, unstable-snapshot handling and lazy Undefined-to-General upload. Sparse holes bypass normal mapping/authority registration.
- Preserve current sampled tile-width spacing, backing/view layer metadata, mipmaps and backing promotion. Preserve all #476 texel-buffer specialization, numeric/bit identity and shifted-handle code unchanged.

## Essential validation

Passed on the combined production source tree:

- Regional authority suite with real lavapipe, production FindOrCreate, region/alias/synchronization methods, production TrapManager/IntervalMap and synchronization validation: **zero Vulkan validation errors**. Repeated with **ASan/UBSan**, also zero errors.
- 24 principal trap/reuse/pending-source permutations across Free Guest OFF/ON, including A→B→A partial writes, already-Clean B and exclusive GPU-only bytes. Shared component pages never gain extra discards after registration. Pre-registration eviction uses actual MADV_REMOVE and is restored.
- Fragmented/later-segment mappings, masked texels versus padding, fully superseded writers, late exact aliases, failed readback/upload and retry, trap contention, current backing/view layer separation, and prospective mip-growth dependencies.
- Exact early production preparation guard and complete render-pass preparation method: stale retained sampled/RT bindings request reconciliation before nodes; coherent reuse adds no submissions. Host pipeline/node facades are explicit boundaries.
- Existing production backing/alias suite: pending receipts/unwind/source lifetime/CPU-newer/failure/retry with Free Guest OFF/ON; alias history/retirement and promotion including 1-to-8 mip growth. Zero synchronization-validation errors.
- Production guest-readback publication/failure/concurrent-trap suite and submission/FenceCycle lifetime/ordering suite: UBSan.
- Sampled-layout/alias and production Linear/Pitch/Block encoding checks: ASan/UBSan.
- Diff whitespace/conflict checks relative to master; source review confirms unchanged sparse/texel/trap/memory implementations and no imported V3 or #400 architecture.

Runners use Clang 19 and the pinned Vulkan-Hpp/C headers. A host libstdc++ shared_ptr-atomic deprecation is left visible as a warning without changing Android-compatible production atomics. LeakSanitizer is disabled in this environment (`detect_leaks=0`); ASan/UBSan and explicit source/resource lifetime checks remain enabled.

## Device and build limits

No Android gameplay, visual improvement, FPS, frametime or RAM result is claimed. The host regional content oracle uses native RGBA8, real Vulkan commands/fences and shared mmap/mprotect/MADV_REMOVE memory. Android allocator/scheduler/construction/context and draw-node/pipeline transport still have explicit host facades; ARM64 NCE signal handling, full shader sampling and UE4 HDR rendering are not executed by these tests.

Free Guest remains enabled as an option, but #393 deliberately retains shared pages in regional components, potentially increasing guest RAM. Converted/lossy GPU writers retain the existing explicit rejection boundary. No new eviction, format-conversion or cross-channel serialization design is introduced.

The existing Release workflow retains master-required gates, assembles only ARM64 DevRelease, excludes the Vulkan validation layer and verifies ELF architecture and APK signature. Build completion/artifacts are not monitored, respecting the user's preference. Test SpongeBob Cosmic Shake and Mario Kart 8 Deluxe on the device before judging visual benefit; keep the PR experimental.

## Build gate follow-up

Run `45283339875` stopped before Android Assemble: the TIC fixture extracted the descriptor-preparation lambda but omitted its enclosing `requireCoherence` helper. Extract that exact production helper into the fixture's enclosing scope as well. No production source, workflow gate, shader submodule or APK configuration changes.

Only the affected TIC fixture was rerun: 1,186 descriptor/specialization checks passed; cache/Vulkan-lifetime mode passed 130 checks and retained ten pipeline variants while prior fences remained pending, with zero Vulkan validation errors. Both modes used the existing ASan/UBSan settings. Previously completed regional, readback, promotion and submission suites were not repeated. Request another ARM64 DevRelease build and do not monitor its execution.
