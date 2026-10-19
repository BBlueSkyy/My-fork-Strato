# GPU-selected compute texture handles

## Demonstrated cause and implementation

The captured MK11 compute shader `0x55736CD732068324` loads a register through
LDG/`LoadStorage32`, ORs it with CBUF6[0], then samples with that register.
An address dependency on CBUF0[0x260] is not the loaded handle. The earlier
semantic-provenance fix prevented the false CPU descriptor but could not
execute the true access. Both changes are preserved: representable CBUF handles
still take the existing static path, while opted-in compute shaders keep the
complete GPU SSA expression (loads, bitwise transforms and Phi selection).

The compiler emits per-operation helper functions before the main function,
so added control flow does not change existing Phi predecessor labels. Vulkan
set 0 remains the static descriptor layout. Regular set 1 contains separate
samplers, a read/write metadata SSBO, and separate Float/Uint/Sint sampled-image
arrays. The GPU splits the actual handle into TIC and TSC, applies the current
ViaHeaderBinding mode, checks bounds and validity, then uses a typed compact TIC
map. NonUniform decorations cover indices, descriptor pointers, loaded resources
and the combined sampled image. Integer results preserve register bits.

The metadata ABI and reproducible production-kernel tests are documented in the
compiler's `tests/dynamic_textures` and `tests/dynamic_vulkan` directories.
No CPU reads the handle SSBO to choose a texture. There is no index clamping,
TIC/TSC-zero replacement, dummy sampler or game/hash-specific execution path.
Invalid or unsupported dynamic descriptors record the first fault atomically
and return zero **without accessing any image/sampler**. The runtime emits an
explicit diagnostic after fence completion. This guard prevents invalid Vulkan
access; it does not claim to emulate an unsupported resource.

## Runtime integration and lifetime

`DynamicDescriptors` enumerates coherent TIC/TSC metadata only for pipelines
that actually contain dynamic accesses. It validates format/shape and sampler
support, creates compact typed image arrays, and enforces actual combined
static/dynamic descriptor and buffer limits. Layout capacities grow by powers
of two; an existing compatible pipeline variant is reused. Ordinary static
pipelines do not scan pools or allocate set 1.

Pool bytes and GMMU mappings are checked again, rather than treating register
dirtiness as a memory-content epoch. Same-size rebinding, content changes and
full requested-image mapping changes (including tail mappings) are handled.
All cached indices sharing an erased TIC view are invalidated together, and
queued views retain ownership. Static lookups retain their live pool spans;
dynamic snapshot overrides are local and restored even on exceptions. QMD
TIC/TSC invalidation flags dirty the appropriate existing caches.

Snapshots are immutable while in flight, including writable fault metadata.
Changed views/backing generations create a new snapshot with unchanged initialized
descriptors copied and changed entries written. Invalid sampler holes are never
copied or written. Unchanged dispatches in the same fence cycle perform no
Vulkan descriptor updates. Completed unchanged snapshots are reset and reused;
a new in-flight cycle needs separate fault storage and a copied descriptor set.
Images, views, samplers, descriptor pools and metadata survive through the fence.

Dynamic candidates are attached and transitioned to GENERAL before static
binding preparation, so shared sampled images capture their final layout.
Existing resource read barriers remain active. A compute-write to host-read
barrier makes fault metadata visible for the fence-delayed diagnostic; it adds
no CPU wait for the fault result, WaitIdle, global idle or timeout. Already
GENERAL images skip `TransitionLayout`, whose existing implementation otherwise
waits on the image fence even for a no-op. A real layout change can still incur
that existing resource-specific wait; eliminating it requires broader executor
transition work outside this integration.

## Required Vulkan features and supported subset

The runtime checks the effective application/device API version and enables
all required bits together: `shaderSampledImageArrayDynamicIndexing`,
`shaderSampledImageArrayNonUniformIndexing`, `runtimeDescriptorArray` and
`descriptorBindingPartiallyBound`. Strato requests Vulkan 1.1, so the descriptor
indexing extension is required even when the device supports Vulkan 1.2+.
The corresponding SPIR-V extension/capabilities are emitted only for dynamic
pipelines. Update-after-bind and variable descriptor counts are unnecessary for
these immutable, fixed-capacity layouts. Existing backend capabilities and
static shader paths remain in place.

This implementation supports non-depth, single-sample 2D **compute** sampling,
explicit/implicit LOD, gradients, gather, samplerless fetch and dimension queries,
with Float/Uint/Sint images. Dynamic depth, arrays/cubes/3D, texture buffers,
storage-image writes/atomics, sparse access and graphics stages are explicitly
unsupported. Static operations for those types retain their previous behavior.
The shared floating-point sampler cache cannot express integer border colors:
dynamic border samplers are therefore rejected, along with unsupported address,
comparison, unnormalized-coordinate and feature/limit combinations. Invalid
entries may remain in a pool; they fault only if selected by the GPU.

GPU-authored handles are supported; GPU/queued writes to the **TIC/TSC metadata**
itself are a separate limitation. The runtime rejects tracked dirty/sequenced
pool ranges before CPU copying, rather than synchronously reading GPU-written
handles or inventing a resource. An implementation of GPU-authored pool metadata
would need a coherent descriptor-materialization mechanism. Devices missing the
indexing features retain static support and explicitly reject dynamic shaders.
There is no universal fallback implemented for those devices.

Turnip source inspected during the preceding work exposes the required indexing
features, but this is not proof for a particular installed driver. Proprietary
and Turnip builds must be judged by the enabled feature bits and actual limits.
Subgroup-waterfall and static-switch alternatives were evaluated on software
Vulkan during design; neither is integrated as a substitute for missing features.

## Performance and remaining work

Pool metadata is scanned and candidate resources prepared conservatively, since
handles can differ between invocations and are not known to the CPU. Samplers,
images, views and compatible layouts are cached; format features are cached by
format. No thousands of resources are recreated per dispatch. However, scanning
cost scales with pool length and all valid candidates must be resident. A new
in-flight cycle currently copies initialized descriptors to isolate fault words.
Dirty-page membership caching and more extensive completed-snapshot recycling
remain optimizations; no Android performance measurements are claimed.

The earlier Eden/Yuzu comparison found CBUF descriptor reconstruction/arrays,
which alone do not implement arbitrary SSBO handles. Ryujinx's preserved
`BindlessElimination.GenerateBindlessAccess` demonstrates separate GPU-selected
texture/sampler arrays. This integration adapts that architectural principle to
Strato's executor and typed views, without copying resource clamping or operation
removal. Exact reference revisions remain in the compiler provenance report.

## Validation and Android artifact

The shared compiler host build passes six suites: subword writes, real
TexturePass provenance/static preservation, image fetch/query, numeric component
types, storage-image capabilities and dynamic kernels. They generate/validate
946 SPIR-V modules, including 18 new dynamic modules for SPIR-V 1.0/1.3/1.4.

Fifteen production-compiler kernels execute on Mesa lavapipe (LLVM 20.1.2,
Vulkan 1.4.318) with Khronos Validation Layer and an error-counting callback:
38,550 result checks pass with zero validation errors. Tests exercise divergent
64-lane SSBO handles, Float/Uint/Sint textures, different samplers/mips, transforms,
Phi selection, table remapping between dispatches, ViaHeaderBinding, samplerless
operations and explicit invalid-handle/filter-pair faults. The gather-offset
modules are structurally validated, not executed by this fixture.

Strato host tests compile the real runtime bodies and pinned Vulkan-Hpp API,
cover feature negotiation, snapshot reuse/deltas, invalid holes, backing
changes, pool/mapping changes, fence lifetime, cached set binding, layout/limit
construction, dispatch order and failure paths. These tests use engine/device
facades; they are not an end-to-end Android execution of Strato's runtime.

The full original Maxwell compiler rebuild and private replay also pass:
seven GPU-sourced bindless operations preserve their handles and operands, while
one legitimate CBUF7:0 fetch retains a static descriptor. One dynamic group and
one static texture descriptor are produced; the complete 11,943-word module
passes `spirv-val --target-env vulkan1.1`. The replay declares the independent
int64 and typeless-image load/write capabilities required by the original shader.
The capture and generated module remain private; no game binary is committed.
The device acceptance test remains MK11 v1.0.0 on the Moto G100: menu/gameplay,
installed-driver behavior, all resource shapes selected in practice and runtime
performance are not established by host tests.

PR #365 keeps its #363 base, scheduler #362, compiler #8/#9 and all earlier
Vulkan corrections. Host gates precede a single ARM64 `:app:assembleDevReldebug`
artifact, `strato-pr365-reldebug.apk`, with Gradle cache and CCache. No clean,
second APK variant, merge or Actions monitoring is required.
