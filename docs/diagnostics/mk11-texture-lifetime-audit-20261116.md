# MK11 TextureManager retention: lifetime audit and blocked eviction

Date: 2026-11-16. Reference master: `650aae297c93510fd119dffb39d0e329602dc008`.
Diagnostic baseline: PR #461, `b1060e0f41725b84f317059b5fcb00285f531d59`.

**Decision: no texture eviction is enabled.** The captures prove growing retained
representations, not which guest allocations have ceased to matter. Source
inspection also exposes destruction prerequisites that must be corrected and
validated before releasing previously permanent manager ownership.

This follow-up adds only this report, isolated host probes and a diagnostic-only
CI job. It changes no Android source, allocator, scheduler, swapchain, shaders,
Free Guest Texture Memory option, or flags/authority. PRs #441 and #457 remain
independent and untouched. No functional PR or merge is justified yet.

## Recovered evidence

The current master is the supplied reference and includes the #362 scheduler
and swapchain merge. PR #461 is still Draft at the supplied diagnostic baseline.

The [first capture](https://github.com/BBlueSkyy/My-fork-Strato/pull/461#issuecomment-6929205767)
reported 2,348 to 7,115 live textures, zero destroyed textures and roughly
+585 MiB VMA allocation between ~90 and ~210 seconds; RSS increased ~1.03 GiB.
The [second capture](https://github.com/BBlueSkyy/My-fork-Strato/pull/461#issuecomment-6930318375)
reported these intervals:

| Metric | ~95 s | ~220 s | Change |
| --- | ---: | ---: | ---: |
| RSS | 5,266 MiB | 6,408 MiB | +1,142 MiB |
| VMA allocated | 1,385 MiB | 2,064 MiB | +679 MiB |
| VMA images | 527 MiB | 956 MiB | +429 MiB |
| VMA buffers | 858 MiB | 1,108 MiB | +250 MiB |
| Texture objects live | 2,339 | 7,304 | +4,965 |
| Mapping entries | 2,334 | 7,299 | +4,965 |

The last classification was 7,298 active, one replaced, zero retired, zero
stale, zero read-only, and no truncation. Neither capture destroyed textures.
Game log messages unload arena slot files; they are not verified kernel/GMMU
free events. Both captures end in device loss, which does not establish OOM
or prove that retention caused the fault. RSS cannot be decomposed by simply
subtracting VMA totals because the counters cover different, overlapping costs.

## Ownership and future-use audit

All source references below use the pinned master, not moving branch contents.

| Path / class | Audited behavior | Consequence for recovery |
| --- | --- | --- |
| [TextureManager header](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture_manager.h#L19) | Each sorted TextureMapping owns a shared_ptr<Texture>; its iterator points into guest.mappings. | All entries belonging to a reclaimed texture would need coordinated removal. One entry is not one image and removing one reference is not destruction. |
| [FindOrCreate](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture_manager.cpp#L17) | Skips replaced/retired during selection; creates new textures, mirrors/traps and entries; no erase implementation. | The prose comment describing deletion in step 4 is stale; executable code does not implement it. replaced=false only means not excluded by those flags. |
| [TIC store](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/interconnect/common/textures.h#L41) | textureHeaderStore independently owns shared_ptr<TextureView> variants; its fast cache stores raw view pointers. | Erasing manager entries alone may free no images. Clearing the strong store independently can invalidate fast-cache pointers and pending descriptor users. |
| [TIC lookup](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/interconnect/common/textures.cpp#L292) | A same-sequence cache hit returns the raw view directly; store hits bypass translation unless replaced. | Mapping invalidation must coordinate these bypasses as well as manager lookup. MarkAllDirty only dirties the pool and does not erase historical store entries. |
| [TextureView](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/texture.h#L328) | View holds a strong lockable pointer to Texture; Texture owns Vulkan views and backing generations. | External views must remain valid; last-owner destruction and cache detachment are separate events. |
| [AliasGroup](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/alias_group.h) | Members/writer are weak, writerIdentity is non-owning; generation encodes outstanding guest authority. | Use count or a weak writer disappearing cannot establish preserved bytes. A last authoritative image may still be essential to stale siblings. |
| [RegisterAliases](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/texture.cpp#L938) | Retires only historical incompatible representations after explicit preservation checks. | Retired is stronger than replaced, but not proof that a Vulkan image is idle or every cached view is detached. The MK11 capture contains none. |
| [Guest traps](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/texture.cpp#L183) | Callbacks acquire weak Texture ownership, track CPU accesses and may wait/retry against changed cycles. | They are coherence callbacks, not allocation free notifications. A CPU write can touch only part of an overlapping allocation. |
| [Overlap index](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/overlap_index.h) | Non-owning augmentation assumes insertions in Update; Rebuild resets the indexed/pending state. | Any future erase must rebuild or implement removal correctly; calling incremental Update after shrinking violates its assertions/invariants. |

### Physical memory identity versus a GPU virtual unmap

[AsGpu::FreeMappingLocked / UnmapBuffer / Remap](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/services/nvdrv/devices/nvhost/as_gpu.cpp)
change GMMU virtual translations. Texman keys are already translated CPU-address
spans; a GPU VA disappearing does not imply that those spans are unreachable
through other GPU VAs, another channel/address space, CPU access or a later remap.

The generic [FlatAddressSpaceMap](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/common/address_space.h#L24)
has an optional unmapCallback. It is not a usable last-physical-use event:
MapLocked and UnmapLocked invoke it with VA/size, under blockMutex, after editing
the block vector. It does not convey the overwritten physical spans, all aliases,
allocation generations, or pending command leases.
[AddressSpaceContext](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/soc/gm20b/gmmu.h#L28)
default-constructs its GMMU and installs no texture lifecycle callback.

[NvMap::FreeHandle](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/services/nvdrv/core/nvmap.cpp)
tracks handle duplicates/pins and SMMU mappings; it does not notify Texman.
A freed handle does not alone identify a unique physical allocation generation
that no CPU/GPU alias can reuse. It is not safe to add a direct eviction call there.

[Kernel MemoryManager::UnmapMemory / FreeMemory](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/kernel/memory.cpp#L801)
changes CPU metadata and discards pages with MADV_REMOVE; no texture lifecycle
transaction is issued. CreateMirror/CreateMirrors use mremap to alias the backing.
A non-null span and MappingsValid only establish pointer/span shape, not current
allocation ownership or a new generation at the same address.
PinGuestMemory only increments texture-local pins and retains a shared Texture;
it is not a general kernel allocation lease.

A useful future event must distinguish GPU-VA detachment from actual physical
allocation discard/reuse, retain old physical identity, cover multiple mappings
and contexts, and prevent old readback/trap cleanup from modifying a new
generation. No such end-to-end event was found in these inspected paths.

## Concrete destruction prerequisites

### Duplicate deletion of a registered trap

The [current destructor](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/texture.cpp#L754)
calls SynchronizeGuest(true) and then DeleteTrap(*trapHandle).
[SynchronizeGuest](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/texture/texture.cpp#L1319)
itself calls DeleteTrap for a Clean texture or after successful GpuDirty readback
when cpuDirty=true and skipTrap=false. It does not clear the optional handle.

[TrapManager::DeleteTrap](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/common/trap_manager.cpp#L36)
dereferences the handle, then IntervalMap::Remove erases its std::list node.
A second deletion is not idempotent: the saved iterator is invalid. This is a
source-proven prerequisite defect; it is not identified as the cause of the
MK11 device loss (no Texture destructions were observed).
It has not been fixed incidentally in this diagnostic change.

The prepared probes extract both production methods unchanged and count
deletion requests using an explicitly instrumented trap facade. They never
dereference an invalid iterator: passing under ASan does not mean the real
duplicate deletion is safe or that a real TrapManager UAF was exercised.

### A cycle pointer is not an image lifetime attachment

[CommandExecutor::AttachTexture](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp#L480)
retains locked textures during execution.
[SubmitInternal](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/interconnect/command_executor.cpp#L709)
orders previous cycles, synchronizes textures and sets Texture::cycle. Its
comment explicitly relies on views retaining the texture itself; it does not
unconditionally attach each texture to that cycle. FenceCycle can retain objects,
but only objects explicitly attached enjoy that guarantee.

The destructor's CpuDirty and early-return paths do not explicitly wait for
pending image use. A FenceCycle destructor waits only when its last owner
releases it, which need not be when a Texture releases its cycle member.
The probe deliberately keeps the cycle alive externally to expose this
obligation. This is not a reproduced Vulkan use-after-free; a future collector
must prove independent retention at every sampled/storage/RT/copy/recorded
descriptor path or defer destruction using an appropriate ownership holder.

### Readback return and retired flags are insufficient certificates

SynchronizeGuest may return without encoding because an alias is stale/read-only,
guest metadata is absent, layout is undefined or host conversion is irreversible.
For a non-group texture it can also change dirty state before discovering that
format conversion is unsupported. Merely returning from SynchronizeGuest is not
a universal proof of recoverable bytes. Partial overlaps need regional authority
preservation; one readback must not overwrite a newer overlapping writer.

Retired aliases are a potential *logical* category for eventual cache detachment
after authoritative preservation, with zero candidates in the MK11 capture.
They are not proven *physically destructible* by this audit: pending views,
traps, reconciliation races and submission ownership remain to be covered.
#457 already fixes retirement/reconciliation and failed-wait staging lifetime
inside its own branch. Those changes are not present in the audited master and
are not copied, modified or presumed merged here.

### Framebuffer and callback coordination

Fixed [framebuffer-cache keys](https://github.com/BBlueSkyy/My-fork-Strato/blob/650aae297c93510fd119dffb39d0e329602dc008/app/src/main/cpp/skyline/gpu/cache/framebuffer_cache.h)
retain raw VkImageView handles.
SwapBacking and backing promotion retire old view-dependent framebuffers and
preserve in-flight objects. Texture destruction does not execute the same
RetireViews protocol. Existing cached framebuffers/descriptors cannot be assumed
safe for future reuse after arbitrary image destruction or handle recycling;
GPU completion and removal from lookup are separate obligations.

TrapManager::HandleTrap invokes read/write callbacks under trapMutex; each callback
temporarily promotes a weak Texture pointer. If removing cache ownership makes
that callback the last owner, destruction can attempt DeleteTrap while still
inside trapMutex. This last-owner/reentrancy path needs a deterministic real
TrapManager test before enabling eviction. No runtime deadlock is claimed to
have been reproduced in this session. Lock order must also avoid calling GPU
recovery while holding a GMMU block lock, kernel memory lock or alias metadata lock.

## Recovery decision by category

| Category | Decision at this checkpoint |
| --- | --- |
| Valid/reusable textures | Keep; no proof of irrelevance. |
| replaced only | Keep; selection exclusion does not preserve data or complete GPU work. |
| retired incompatible alias | Candidate only; fix/test destruction prerequisites and fence ownership first. Zero in this MK11 trace. |
| Historical authoritative representation | Keep until exact guest/alias authority is preserved. |
| GMMU-VA unmapped but physical spans may survive | Keep; VA unmap is insufficient. |
| Kernel allocation discarded/reused | No safe transaction/generation check exists in the audited paths. |
| Manager-only reference by use_count snapshot | Reject; neither future guest use nor concurrent weak promotion is excluded. |

No category is newly released. Mappings removed, textures destroyed and Vulkan
bytes freed by this follow-up are all zero. No RSS, CPU, FPS or frametime benefit
is claimed. No periodic scan, draw collector or wait-idle is added.

## Prepared tests and remaining acceptance gates

Run the narrow prerequisite probes with:

```sh
CXX=clang++ python3 tests/gpu_texture_lifetime_audit/run.py --expect-master-blockers
```

The runner extracts the current production SynchronizeGuest and destructor bodies,
compiles them with ASan/UBSan against OS/GPU/trap facades, and prints their source
SHA256. Nine scenarios cover CpuDirty, Clean, GpuDirty readback and trap deletion,
an externally retained pending cycle, stale/read-only alias guards, converted
encoding refusal, external ownership and grouped guest-generation publication.
**Success means known blockers were observed, not that GC is safe.**
The diagnostic workflow is restricted to the #461 branch.
The facades simulate waits, copies and OS traps; they do not validate actual
Vulkan barriers, kernels, the full class layout or real TrapManager concurrency.

The local execution environment was unavailable during preparation.
No local compile, sanitizers, Vulkan validation or performance measurements
were possible. CI outcomes must be recorded separately after reading the actual
run result; earlier PR validation is context, not a new pass.

| Required future case | Concrete acceptance gate |
| --- | --- |
| Creation/reuse | Same guest allocation generation returns matching backing without duplicate image allocation; a new generation cannot reuse stale data. |
| Replacement | Old authoritative bytes survive; replaced alone cannot authorize eviction. |
| Unmap/reuse | Two GPU VAs and two contexts map the same physical range; first unmap keeps data, last physical discard handles pending users; remapping same address with new generation never receives old readback. |
| Partial overlap | Parent mip1, child mip0 and adjacent-only/later mapping preserve newest regions independently; include MK8 HDR overlap and SMO layout cases. |
| Pending GPU writes | Hold a real Vulkan fence unsignalled with a test-only timeline semaphore, remove lookup owners, verify image/view/staging survive until completion; cover failed wait. |
| Cached views/descriptors | Exercise same-sequence TIC raw-pointer hit, store hit, cached descriptors, RT framebuffer reuse and handle recycling after invalidation. |
| GPU to guest | Compare every encoded byte and serial/generation; unsupported encodings must reject release. |
| External references | Erase all manager mappings while a view/descriptor/copy retains the resource; defer physical free until all uses finish. |
| Invalidation/access race | Deterministic barriers around trap weak promotion, final-owner removal, GMMU translation, kernel reuse and alias reconciliation; assert no deadlock, revival or stale write. |
| Preservation after recovery | Recreate representation and compare native, converted, multi-mapping and partially overlapping contents. |

Reuse tests/gpu_backing_promotion (actual manager/alias methods with real Vulkan),
tests/gpu_texman_multirange, tests/gpu_sampled_layout, gpu_framebuffer_cache and
gpu_submission as starting points. They do not by themselves cover a nonexistent
eviction protocol. Run ASan/UBSan plus Khronos core and synchronization validation
on the actual new release protocol before publishing a functional PR. A complete
suite, including real TrapManager and guest allocation generation behavior, remains
pending.

Any future recovery diagnostics must separately count lookup removals, final
Texture destruction, VMA live allocation bytes, VMA reserved blocks, deferred
GPU/alias/external owners, RSS and measured cold-path cost. A dropped shared
reference is never reported as vkFreeMemory or RSS reclamation.

## Next implementation boundary

First establish safe exactly-once trap cleanup and callback-final-owner behavior
with real trap tests. Separately define an allocation-generation/physical-lifetime
event and independent retention of all submitted/recorded resources, including
descriptor and framebuffer caches. Only then select the smallest demonstrated
reclaimable category. Do not infer that unloading an arena frees kernel memory;
an engine may retain a guest heap and only change suballocations.

The next device test should validate a concrete lifetime correction or collect a
specific missing lifecycle event, not another round of general counters.
Until then, keep #461 diagnostic, #441/#457 independent and #362 intact.
