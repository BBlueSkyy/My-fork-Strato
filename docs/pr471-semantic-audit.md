# PR #471: split bindless CBUF semantic audit

Audited starting commit: `6253a15dcd4b6244b92b7f2a792ceb6791234116`.
Pinned shader compiler: `9cdd405f861229420a57f79832bf68b65c219fe6` (unchanged).

## Conclusion

Keep zero for an absent secondary **word**, preserving the primary word and
both shifts. This is a compatibility policy consistent with this fork's shader
specialization. It is not established here as a measured Maxwell/NVN hardware
guarantee. Reaching gameplay demonstrates the crash fix, not hardware fidelity.
Handle zero also means TIC/TSC index zero; it is not automatically a null texture.

Two additional inconsistencies are reproduced and fixed:

1. An absent primary previously returned the entire handle as zero, even when
   the secondary was valid. Specialization composed each word independently.
2. Descriptor resolution used ordinary `ConstantBuffer::Read`, whereas shader
   specialization used the bounds-checked, retryable `ReadForShaderCompile`.
   On current-cycle GPU-dirty data, the former only logged its flush callback;
   staged/direct buffer read paths could then observe old data or wait without
   the needed submission. It also lacked primary view-range validation.

Both handle words now use the existing retryable reader. A bound but truncated
view remains an explicit error; no fallback for corrupt or partially mapped
bound data was added.

## Safe descriptor ordering

Simply enabling retry at the old read locations would be unsafe:
`GetStorageBufferBinding` / `GetImageBufferBinding` can mark a shared buffer
GPU-dirty before the later handle read. This draw has not been queued yet;
retrying and repeating those bindings would mark it dirty again without progress.

Full graphics, graphics quick-bind and compute now resolve their handles once,
before any resource-binding callbacks. Each callback consumes the corresponding
saved handle in the same descriptor/array order. Graphics snapshots cover all
active stages before binding any stage's outputs, including cross-stage aliases.
There is no second CBUF read after output binding. Temporary storage uses the
existing command allocator: four bytes per updated image/texel-buffer descriptor.

The existing `RunWithTextureBackingRetry` boundary unwinds locks and the local
builder before submission. Graphics submission callbacks invalidate pipeline and
descriptor state and disable quick bind; compute purges pipeline state. No draw
or dispatch command has been enqueued at these handle-read sites.

## Origin of stage 4 / CBUF 16

The diagnostic `emulation(20261120-120631).log`, lines 287-295, confirms a combined
image/sampler descriptor requesting secondary CBUF 16 at offset zero, stage 4,
with `bound=false`, immediately followed by `ConstantBuffer::Read` and SIGSEGV
at `0x10`. Stage 4 is fragment; slot 16 is within the 18 graphics CBUF slots.

The command decoder forwards each of the five bind groups' `shaderSlot` and
`valid` fields directly. `ConstantBuffers::Bind` assigns the selected view to
the indexed stage/slot and rejects an unmapped selector. `Unbind` clears that
slot. No binding-loss defect or stage/slot truncation was demonstrated.

In the pinned compiler, `TryGetConstBuffer` recognizes two CBUF origins joined
by `BitwiseOr32` and carries their indices, offsets and shifts into texture
descriptors. The new production TexturePass test reproduces a secondary
`c[16][0]` from `c[2][0] | (c[16][0] << 20)`, with matching metadata origins.
The runtime fallback does not create a binding for slot 16.

**Limit:** the log contains neither bind/unbind history nor the offending shader
binary/IR. The synthetic test proves the compiler route, not that exact expression
in the game's shader. Initial absence, explicit guest unbind, an unexecuted
shader path, and an erroneous origin analysis cannot be distinguished from this
log. No speculative binding or compiler changes were made.

## Specialization and descriptor coherence

Compilation and `MatchesSpecialization` resolve split-handle sources using
`ReadForShaderCompile` and `TextureSource::ReadHandle`. An absent source contributes
zero independently. Metadata queries record the source and queried type/integer/
compare result, rather than freezing every texture handle as scalar code data.
Rebinding can reuse a variant if metadata matches; changed metadata rejects it.

Both primary and secondary CBUF usages are registered for sampled and texel-buffer
descriptors. A single secondary bind therefore updates the affected descriptors;
unbind/load disable quick bind and force a full refresh. The new tests cover slot
16 bind/unbind/rebind and variant reuse. Shader predicate evaluation is unchanged.
Eager descriptor preparation may inspect resources in paths the GPU never executes;
its success cannot prove an executed unbound Maxwell CBUF load returns zero.

## External comparison and evidence limits

- [Eden Vulkan descriptor reads](https://github.com/eden-emulator/mirror/blob/467ab6441ad928213c2adb666420fc8a4f954e9b/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp)
  assert that both CBUFs are enabled, then combine their words. Eden's
  [shader environment](https://github.com/eden-emulator/mirror/blob/467ab6441ad928213c2adb666420fc8a4f954e9b/src/video_core/shader_environment.cpp)
  also asserts enabled and returns zero outside the declared size. These are
  emulator policies, not hardware verification of #471's absent-bank case.
- The indexed [Ryujinx-derived `TextureHandle.ReadPackedId` source](https://git.dukantic.fr/dukantic/nxnative_patch/src/branch/borderlands/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/TextureHandle.cs)
  independently substitutes zero for a primary/secondary word outside its cached
  span and combines their contributions. Full source retrieval was unavailable;
  this comparison is limited to the indexed method, not an audit of that fork's
  binding/memory synchronization or proof of Maxwell behavior.
- [D3D11.3 section 7.5](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#ConstantBuffers)
  defines zero reads from absent constant buffers at the API level. This does
  not establish the semantics of raw Maxwell/NVN disabled-bank accesses.

## Validation

- New host cases on the starting implementation: **1,174 checks, six failures**
  (primary absent/secondary valid, specialization parity, pending reads on either
  source, and primary validation before touching the view).
- Final host suite: **1,186 checks, zero failures**. Cache probe: **95 checks,
  zero failures**, including 20,000 equivalent-handle refreshes without a new
  compile or flush in the host facade.
  Tests execute the production reader and the full/quick/compute snapshot passes
  with ASan/UBSan; allocation, backing and output dirtiness are facades. They
  cover both-absent/both-bound combinations, shifts, arrays, short views,
  descriptors without a secondary field, all descriptor categories, two shader
  stages, pending reads/retry, and empty descriptor lists.
- Pinned TexturePass: synthetic split handle and the existing bound/bindless
  eight-target checks pass under UBSan. Shader compiler source/pin is unchanged.
- Production preload and default/overridden texture-query callback regressions
  also pass. The latter emits an existing GCC warning for a Clang diagnostic
  pragma in the pinned compiler header.

Commands: `python3 tests/gpu_tic_validation/run.py`, the same command with
`--probe-cache`, `python3 tests/gpu_tic_validation/run_shader.py --deps app/libraries`,
`python3 tests/gpu_tic_validation/preload/run.py`, and
`python3 tests/gpu_tic_validation/replay/test_query_callbacks.py --deps app/libraries`.
This host supplied Boost headers via `CPLUS_INCLUDE_PATH` for compiler tests;
fmt/range and the shader compiler were initialized at their pinned revisions.

No new instrumentation, validation-layer build, Android APK build, GPU execution
test or on-device gameplay retest was performed. The previously working
primary-valid/secondary-absent handle value is preserved by executable tests.
No claim of unchanged device FPS or resolved unrelated graphical defects is made.
