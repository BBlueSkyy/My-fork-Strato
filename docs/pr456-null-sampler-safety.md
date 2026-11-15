# PR456 — null sampler safety follow-up

Baseline: `fe4c33f313b1900f534b562a02984b9a50afa449`. This follow-up changes
only the sampled-null binding in `common/pipeline.inc`, its regression tests
and this evidence. The compiler, pipeline caches, storage policy and previous
checkpoints are unchanged.

## Confirmed defect

`Samplers::GetSampler` returns `nullptr` for an out-of-pool TSC. The ordinary
sampled-null path in `GetTextureBinding` then emitted a combined descriptor with
both `sampler` and `imageView` null. Graphics full/quick updates and compute all
forward this result as `eCombinedImageSampler`. Their layout bindings omit
`pImmutableSamplers` (null by default); no immutable sampler protects this path.

[Vulkan VUID-VkWriteDescriptorSet-descriptorType-00325](https://docs.vulkan.org/refpages/latest/refpages/source/VkWriteDescriptorSet.html)
requires a valid sampler for a combined descriptor without immutable samplers.
`nullDescriptor` permits the null image, not a null sampler.

The minimal correction obtains the existing cached `GetNullSampler` only when
this null-image branch lacks a guest sampler. A valid guest sampler remains
unchanged. Allocation failure remains an exception; the Samplers instance owns
the Vulkan sampler as before. No new resource destruction or per-draw allocation
is introduced. A reserved TIC without nullDescriptor remains rejected. A valid
TIC0 is not converted to null. Other guest-backed image paths are unchanged.

## Executable evidence

- Production TIC/binding fixture, ASan/UBSan: **1,145 checks, zero failures**.
  The added reserved/unmapped-TIC plus TSC `0xFFE` with a one-entry sampler pool
  catches the old null sampler. Before the correction, nine dimensionality
  checks failed. Valid TSCs retain their original sampler; unsupported hosts
  still reject reserved TICs.
- Vulkan fixture now extracts the actual production `GetTextureBinding`, uses
  real VkSampler handles and submits its combined descriptors. Guest TIC
  decoding is tested in the first fixture; the Vulkan boundary supplies its
  `InvalidDescriptor` result. The out-of-pool TSC returns nullptr in the sampler
  facade. Before the correction the explicit sampler-contract assertion failed.
  Afterward seven sampled dimensions and the unchanged storage control dispatch
  successfully, produce 64 zero words, and report **zero validation errors**.
- The host validation layer (API 1.4.309) did **not** report VUID 00325 for a
  deliberate update with both handles null. That exploratory control's expected
  error assertion failed. This is recorded as a limitation of that control,
  not evidence that null samplers satisfy the specification. The final fixture
  checks the required non-null sampler before update and separately requires
  zero validation errors during real dispatch; no reported error is filtered.
- Existing cache/lifetime suite: **52 checks, zero failures**, ten real pipeline
  variants retained while an old fence is pending; zero Vulkan validation errors.
  20,000 equivalent-handle draws still perform zero compilations/Flush calls.
- Pinned compiler TexturePass: 16 bound/bindless target cases pass under UBSan.
  The compiler submodule remains unchanged at `cb0ea66`.
- Existing null-storage policy and sampled-layout/alias regressions pass. The
  first null-storage invocation hit the host's LeakSanitizer/ptrace restriction;
  retry with `ASAN_OPTIONS=detect_leaks=0` passed, retaining ASan and UBSan.
  This is not a leak-check result. Existing host shared_ptr deprecation warnings
  remain in the sampled-layout output.

Commands: `python3 tests/gpu_tic_validation/run.py`,
`python3 tests/gpu_tic_validation/run_vulkan.py`,
`python3 tests/gpu_tic_validation/run.py --probe-cache --vulkan-lifetime`,
`ASAN_OPTIONS=detect_leaks=0 python3 tests/gpu_null_storage_image/run.py`,
`python3 tests/gpu_sampled_layout/run.py`, and the pinned `run_shader.py` gate.
Host Vulkan uses lavapipe plus VK_LAYER_KHRONOS_validation.

This closes the combined-descriptor sampler defect. It does not establish
Maxwell results for an actually executed invalid sample or replace on-device
Kirby and compatibility testing. PR456 remains Draft, without merge. Android
Release assembly and artifact upload are gated by the existing Actions tests;
the APK must not be claimed available before upload is confirmed.

Raw red/green outputs: [preserved results](checkpoints/pr456-null-sampler-20261115/results.txt).
