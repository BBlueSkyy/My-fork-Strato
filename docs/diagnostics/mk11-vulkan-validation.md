# MK11 Vulkan validation follow-up

## Latest integration: GPU-selected SSBO texture handles

The compute dynamic-descriptor integration now supersedes the rejection-only
result below. See [the implementation and validation report](dynamic-bindless.md)
for the supported operations, real Vulkan execution tests, feature requirements,
resource lifetime and remaining limitations. Prior evidence and fixes remain
recorded below; successful menu/gameplay and performance on the Moto G100 still
require device testing.

## Earlier result: memory-loaded bindless handle provenance (verified partial fix)

The new evidence is `mk11_shader_evidencias.zip`, containing the original
5,400-byte Maxwell and complete IR snapshots for compute shader
`0x55736CD732068324`. It demonstrates a compiler value-provenance error:
`LoadStorage32(0, address - aligned_cbuf_base) | CBUF6[0]` was reconstructed as
`(CBUF0[0x260] & 0xfffffffc) | CBUF6[0]`. The SSBO address was mistaken for
the loaded value. The full SSA chain includes identities %142/%212/%219, not
a direct CBUF handle. Original Maxwell independently confirms LDG.E.32 into R8,
followed by OR with CBUF6 and TEX.B consuming R8.

Compiler #9 commit `6f57053773f050a15e6345d1ed34a12801ff5957` replaces dependency BFS with explicit
value transformations and rejects unmodelled provenance. CBUF masks, shifts,
split handles and bounded CBUF arrays remain; selected CBUF-pair lanes retain
the correct word offset. Invalid OR branches cannot be silently discarded.
Collection rejects before texture-type/descriptor lookup or texture IR rewriting.
Legitimate static CBUF1 mask reads on valid sibling expressions remain permitted;
no CPU SSBO readback is added.

**This is a partial correctness fix, not dynamic SSBO-handle support.** The
captured MK11 shader now reports unsupported handle provenance during
translation (the full host replay first encounters a Phi in post-order; the
focused captured-sample regression rejects LoadStorage32). It cannot execute correctly with Strato's current CBUF-only
descriptor metadata and CPU binding path. This APK is not expected to bypass
that missing feature or establish menu/gameplay support.

A complete implementation requires GPU-side TIC/TSC indexing, typed separate
image/sampler tables, real descriptor-indexing feature/limit checks, and
resource residency, barriers and lifetime management. The existing small CBUF
arrays do not provide that architecture. Enabling an extension alone or reading
the SSBO from the CPU would not implement it correctly. No new Vulkan feature,
sampler replacement, pool-index substitution, cache purge or synchronization
was introduced.

See the companion
[provenance report](https://github.com/BBlueSkyy/shader-compiler/blob/6f57053773f050a15e6345d1ed34a12801ff5957/tests/handle_tracking/PROVENANCE.md)
for instruction offsets, SSA, Eden/Ryujinx comparison and the remaining design
requirements. The earlier report below describes previous stages of #365;
its former uncertainty about provenance is superseded by this captured case.
The observed numeric example produces TSC 2029, not a one-to-one reconstruction
of the earlier 1772/2993 handles.

Validation: five compiler CTests pass with Clang and the exact pinned Strato
dependencies; 929 SPIR-V modules validate, including 57 modules emitted after the
real TexturePass. Memory/atomic/alias/variable-offset negative controls reject;
static descriptors and the 256-access deduplication control remain intact.
The complete host compiler builds. A replay of the original Maxwell reproduces
the entire pre-pass SSA after normalizing host pointer prefixes; the old pass
accepts the fabricated 0x7ed15200 handle and the corrected pass rejects before
any texture descriptor query. Strato sampler, bindless-binding and compact
descriptor suites pass. Device rendering and performance remain unverified.

Only the compiler gitlink and this report change on Strato. Scheduler #362,
Strato #363, earlier #365 fixes and the targeted diagnostic captures are
preserved. The existing workflow runs host gates before one ARM64
`:app:assembleDevReldebug` APK, retains Gradle/CCache, and uploads
`strato-pr365-reldebug.apk`. No clean, extra APK variant, merge or Actions
monitoring is requested.


## Baseline and evidence

This branch depends on Strato #363, `5bf1a37572c531b7a3213b749b97113907ae6e60`.
Scheduler #362 and compiler #8 are preserved. The additional compiler work is
[shader-compiler #9](https://github.com/BBlueSkyy/shader-compiler/pull/9), commit
`ee34366c5949450cc244c08fe9c186aadbb6a1cd`, based directly on #8.

The input is `emulation(20261019-022212).log`, with the built-in Validation Layer,
Turnip 25.2.99, Vulkan 1.3.325, Adreno 650. It contains 6,233 storage-view swizzle
errors (00336), 6,197 numeric descriptor errors (02699), 1,548 attachment-overlap
errors (04584), two invalid fetch modules, two cube-array feature errors, and two
null-sampler writes (00325). Later command-buffer errors can be consequences of
invalid updates, not independent evidence of a lifetime defect.

At 58.685 s, GetSampler rejects TSC indices 1772 and 2993. At 58.762 s, validation
rejects null samplers at bindings 9 and 10 of the same descriptor layout. At
58.802 s, Sky-CmdRecord faults at address 0x68 inside the driver while recording
SetDescriptorSetCmdImpl<true>. The code directly confirms that a failed sampler
lookup became VK_NULL_HANDLE in an ordinary combined-image descriptor.
This demonstrates an invalid submission path. It does not identify the exact
shader expression, raw handles, selected pool bounds or binding mode that
produced those two indices, nor prove a sole driver crash trigger.

## Handle and sampler corrections

The production TexturePass and runtime binder had reproducible defects:

* AND masks were discarded and their trailing-zero count was treated as a
  shift. For example, `(0x6ec00021 & 0xfffff) | (3 << 20)` must select TIC 33,
  TSC 3; reconstructing the unmasked source selects TSC 1775 instead.
* A single-source descriptor's shift was ignored by ReadBindlessHandle.
* Descriptor deduplication did not distinguish all source transforms.

The compiler now preserves normalized masks and cumulative shifts and shares
safe handle evaluation with the binder. Shifts of 32 or more have an explicit
zero result instead of C++ undefined behavior. Both sources, arrays and
unbound secondary buffers are covered by tests. No index is clamped or replaced.

ImageFetch and ImageQueryDimensions do not consume a sampler. They now use raw
SPIR-V image variables and VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE. ImageQueryLod,
sampling, gather and gradient operations retain combined descriptors and actual
samplers. Mixed descriptor groups use correctly sized per-binding writes;
graphics full/partial updates, compute layouts, compact metadata and binding
compatibility preserve this distinction. A null texture retains its valid
sampler when a combined descriptor is used.

If an operation really requires a sampler and lookup fails, binding throws with
raw-handle/CBUF context before submitting an invalid Vulkan descriptor. This is
an explicit unsupported/invalid-state error, not an emulation fallback. It may
expose another unresolved source of invalid handles on device. It must not be
reported as proof that the exact MK11 indices are fixed.

ViaHeaderBinding's index/limit/address policy agrees with the inspected Strato,
Eden/Yuzu reference behavior. No evidence justified changing it. Sampler objects
remain in the content-addressed owning store; no sampler destruction or command
slot lifetime cause was demonstrated. Eden's reserved real sampler approach
was not copied, because substituting an arbitrary sampler would hide the
underlying guest handle problem.

## Other demonstrated Vulkan violations

* **00336:** storage image binding obtains a separate identity-component view.
  Sampled TIC mappings remain intact. Both hot caches invalidate with backing
  generations, and the backing's view cache still shares identical views.
* **02699:** sampled images and texel buffers carry Float/Uint/Sint through
  translation, SPIR-V scalar/result types and Strato callbacks. Integer results
  are bitcast to the existing register IR, not numerically converted. Stencil
  views classify as Uint; depth stays Float. Graphics/compute caches retain
  numeric variants and reject incompatible reuse, including transition-cache
  hits. CBUF changes and sampler-pool/TIC generation changes invalidate binding
  decisions; unchanged draws keep the fast path. Pipeline bundle cache version
  changes from 5 to 6 to avoid reusing old boolean type metadata.
* **OpImageFetch Offset:** Vulkan without maintenance8 rejects dynamic Offset
  operands on fetch. Add the signed offset to integer spatial coordinates,
  preserving array layer coordinates and LOD. No optional feature is assumed.
* **Cube arrays:** enable the supported core imageCubeArray feature, instead of
  creating cube-array views without enabling it. Unsupported devices are not
  made to advertise support.
* **04584:** detect overlapping sampled and attachment subresources. Negotiate
  both attachment-feedback-loop extensions and their feature bits, add the
  required image usage, initialize/cache the dynamic aspect mask, patch complete
  sampled descriptor layouts, and bracket a single feedback draw with scoped
  image transitions and a dedicated render pass. Restore layouts afterwards.
  Ordinary draws do not receive these transitions. Local extension ABI structs
  preserve the pinned Vulkan-Hpp/SDK instead of replacing the headers. Usage
  filtering preserves extension bits unknown to that old header version.

The supplied driver advertises both feedback extensions. Unsupported native
feedback, or a storage-image binding sharing a transitioned feedback backing,
is rejected explicitly rather than sent with an invalid layout. This native
path does not invent draw-start snapshots or promise additional ordering between
fragments within a single draw. Guest ordering and Vulkan feedback restrictions
still apply. The log alone cannot exclude distinct storage views aliasing one
of the sampled feedback backings.

## Reference comparison

The comparison separates guest handle semantics from Vulkan descriptor rules.
Yuzu/Eden's handle source tracking and pool selection were useful references,
but their analogous mask tracking is not evidence of correctness; the new
counterexamples exercise the actual compiler pass. Ryujinx distinguishes
samplerless texel fetch from true sampling and has explicit native feedback
feature/state handling. Merely splitting render passes, as found in reference
feedback paths, does not legalize simultaneous attachment and shader access.

Primary references:

* [VkDescriptorImageInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorImageInfo.html)
* [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)
* [Feedback loop layout](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_attachment_feedback_loop_layout.html)
* [Feedback dynamic state](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_attachment_feedback_loop_dynamic_state.html)
* [Preserved Yuzu descriptor binding](https://source.hodakov.me/hdkv/yuzu/src/commit/dd3c156af45c9899bceae3fad0aa83c8dbd0d917/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp)
* [Ryujinx masked/shifted handle work](https://git.axenov.dev/mirrors/ryujinx/commit/25fd4ef10e610ee470b76d6f58b4a3b9cd053844)

## Validation and limits

The compiler's shared standalone suite passes with GCC and Clang 18 using the
exact Strato range-v3 pin for Clang. All 872 generated modules pass spirv-val:
576 subword, 243 fetch/query, 36 numeric-type and 17 storage-image modules.
Real TexturePass tests execute masked/split/nested/zero/large-shift cases under
UBSan. Negative controls reproduce the omitted-mask defect and the original
fetch operand validation error (84 failures, 42 no-offset controls passing).

Strato host suites cover actual compact descriptor constructors, mixed layouts
and writes, required sampler rejection, numeric pipeline variants and cached
transitions, production TIC conversion, stencil, pool generations, bundle
round-trips, identity view caches and backing changes, native feedback feature
chains/RAII calls, dynamic initialization/reset, per-image transitions,
exception cleanup and mixed-storage rejection. Historical/mutated negative
controls fail their intended assertions. Scheduler (22), conditional rendering
(20), DRAW_TEXTURE (16), submission (26), sampled layout (14 upload oracles),
sampler and 64-feature-profile regressions pass.

The existing gpu_backing_promotion fixture still has unrelated compile drift
(pitch helpers, DirtyState facade, alias variables, Extent3D assignments).
Only its view API declaration/cache fields were adapted; no unrelated behavior
was rewritten. New focused image-view regressions cover the affected API.

These are compiler and host boundary tests. They do not constitute GPU execution,
Android APK compilation, zero on-device validation errors or a performance
measurement. No claim is made that MK11 reaches the menu or gameplay. Some
pre-existing storage-image signedness/global-memory/alias limitations remain
outside these sampled-image changes. Required invalid samplers and unsupported
feedback now produce explicit errors, not substituted resources.

The dependent PR builds one ARM64 DevReldebug APK after the host gates, keeping
Gradle cache and CCache without clean. Normal PR variants remain unchanged.
No merges or Actions monitoring are performed. Device validation should first
reuse MK11 v1.0.0, the same driver/settings and Validation Layer to compare the
first remaining violation and the last game initialization milestone.


## Targeted MK11 compute-shader capture (unvalidated on Android)

The device reports compute shader hash `0x55736CD732068324`, GPU address
`0x0000000403F3A000`. Its failing handle uses compute CBUF 6 and CBUF 0.
The shader-compiler #9 diagnostic callbacks and Strato #365 capture only
this one hash **once per emulation**. No global shader dumps, forced handles,
sampler substitutes, cache invalidation, or guest scheduling changes.

Strato emits `MK11ShaderTrace` and `MK11ShaderDumpBegin/Chunk/End` messages
for `maxwell` (original guest bytes), `before_texture_pass`, and
`after_texture_pass`. When a shader replacement is present, the optional
`maxwell_translation_input` phase captures the replacement separately. The
`before_texture_pass` snapshot runs after constant propagation and global
memory lowering but **before** bindless texture lowering; it preserves the IR
expression analyzed by TexturePass. The `after_texture_pass` snapshot shows
the lowering result. They are diagnostic snapshots, not an assertion that
translation was correct. The preexisting `ComputeBindlessTrace`,
`ComputeQmdTrace` and `ComputeQmdCbufTrace` remain in place.

For log portability, each blob is hex encoded in 256-byte chunks with absolute
byte offsets and begin/end markers. Each phase is capped at 256 KiB; the begin
marker identifies any truncation. All trace-specific exceptions are swallowed
so diagnostic logging does not affect normal guest code. With a completed
Android diagnostic run, reconstruct the blobs from an ordinary emulator log:

```bash
python3 scripts/extract_mk11_shader_trace.py emulation.zip mk11_shader_dump
```

If the log lacks `MK11ShaderDumpBegin`, check that the APK contains the
diagnostic commits and actually compiled this hash. The extractor rejects
missing/overlapping chunks instead of writing corrupt output. If a phase is
truncated, its partial blob is **not** enough to conclude that the compiler
decoded the whole shader correctly.

**Validation status:** reviewed source wiring and verified the gitlink points
to the companion compiler commit. Neither a production Android compile nor
device capture is asserted by this report. Keep #365/#9 Draft until the new
log and the shader expression have been analyzed; do not merge based on
diagnostic instrumentation alone.
