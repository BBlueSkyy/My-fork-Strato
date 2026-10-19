# Android shader compiler audit and MK11 validation

## Scope and baseline

The Strato branch is based on scheduler PR #362, commit
`58a1405b502ce371c5719ef1c71fa3cf8f6d1c99`. No scheduler code or temporary
#361 instrumentation is changed. The compiler was pinned to
`dcb938866f4234963fcbc199f2443438d8ef6f07`. Its remote master,
`d5d023b89254b0a6abec572be1bd64fdc63528cf`, is an ancestor, not a newer fix.
The six existing compiler commits after master, including ClipDistance,
typed storage images and aligned U64 constant-buffer accesses, are preserved.
The [compiler PR #8](https://github.com/BBlueSkyy/shader-compiler/pull/8)
uses a snapshot base at the exact original Strato pin so its diff contains only
this work. New pin: `0bf9347778dbd35423b4ce2fdc746c722252bfae`.

## Confirmed gaps and changes

* SSBO U8/S8/U16/S16 stores lacked a fallback. Arithmetic int8/int16 support
  alone incorrectly selected narrow storage accesses. Unsupported profiles
  could emit missing types/pointers or require storage features not enabled
  by Strato. Separate arithmetic/storage flags now control native accesses.
* Unsupported narrow stores use a per-SSBO 32-bit atomic load/CAS helper.
  BitFieldInsert preserves neighboring bytes/halfwords. Failed compare/exchange
  retries from a fresh atomic value. Device scope and relaxed ordering provide
  atomic replacement; guest barriers remain responsible for ordering.
  No optional variable-pointer feature is needed because helpers reference
  their global SSBO directly. No retry timeout or artificial sleep is added.
* If any used subword width needs emulation in a writable shader, both widths
  use word accesses, and subword fallback reads are atomic. This avoids
  overlapping native narrow and emulated word accesses within that shader.
  Entirely read-only shaders retain ordinary fallback loads. Fully supported
  profiles retain native accesses. Unsigned native stores use UConvert.
* Strato queries/enables storageBuffer8BitAccess and storageBuffer16BitAccess
  separately from arithmetic support. Vulkan 1.1 requires the 8-bit-storage
  extension even when the physical device advertises Vulkan 1.3. The supplied
  Turnip log lists int8/int16 arithmetic, but not VK_KHR_8bit_storage.
* Shared compiler/binder SSBO alignment is at least four bytes, and maximum
  descriptor range is word-sized. This covers enclosing CAS words even on
  hardware with native offset alignment one or two; normal >=4 alignment is
  unchanged. Existing low-bit halfword normalization is preserved.
* Typed R8ui/R8i/R16ui/R16i/Rg32ui storage image and texel-buffer declarations
  now emit StorageImageExtendedFormats when used. Ordinary formats do not.
  This SPIR-V capability is allowed by Vulkan core independently of enabling
  shaderStorageImageExtendedFormats; it does not guarantee every actual format
  has storage support. No unsupported Vulkan feature is forced on.
* Typeless image writes now require the actual enabled write-without-format
  profile feature; unsupported shaders fail explicitly instead of declaring an
  unavailable capability. No universal format-conversion fallback is claimed.

## Historical comparison

The original Yuzu #12652 commit was recovered from Eden's preserved history:
[2a0d707ce1d4880dfcbd34c4d6572917a501f675](https://git.eden-emu.dev/eden-emu/eden/commit/2a0d707ce1d4880dfcbd34c4d6572917a501f675).
The January 11, 2024 patch implements subword storage stores using 32-bit CAS.
It does not fix extended storage image formats. The
[Yuzu January progress report](https://yuzu-mirror.github.io/entry/yuzu-progress-report-jan-2024/)
reports MK11 improvements from that work while discussing remaining image
format limitations separately. This implementation adapts the same atomic
replacement principle to Strato's backend, adds explicit storage feature
negotiation, and avoids pointer arguments that require variable pointers.
Eden's preserved implementation supports this attribution; Ryujinx was used
for sampler binding/pool semantic comparison, not as the source of this CAS fix.

Relevant Vulkan references:
[SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html),
[device features](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html),
[memory model](https://docs.vulkan.org/spec/latest/appendices/memorymodel.html).

## Independent descriptor crash investigation

The supplied `emulation(20261018-212812).log` reaches StudioLogos.mp4 and
BGND_MainMenuBackground. It reports invalid TSC indices 1836 and 3505 at
58.377 s, then SIGSEGV in Sky-CmdRecord at 58.626 s, through
SetDescriptorSetCmdImpl<true>::Record into vulkan.ad07xx.so (Turnip 25.2.99).
This establishes neither a shader compiler cause nor the exact descriptor
responsible for the crash.

A separate, reproducible cache bug was fixed: rebinding a same-size sampler
pool to a different mapping retained sampler objects indexed from the old
pool. Update now invalidates that per-index cache when the mapping changes.
The content-addressed sampler store retains existing Vulkan objects and their
lifetime; unchanged pools retain cache reuse. The fix is a separate commit.
It does not explain the out-of-range indices in this log.

ViaHeaderBinding and explicit/linked index rules match the reference behavior
inspected in Eden/Ryujinx. No command-slot or sampler object lifetime defect
was demonstrated. Out-of-range GetSampler still returns nullptr; indices are
not replaced with zero or a generic sampler. A null ordinary combined-image
VkSampler is invalid even with robustness2 nullDescriptor (see
[descriptor validity](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html),
VUID-VkWriteDescriptorSet-descriptorType-00325). The log lacks raw handles,
pool bounds, stage and constant-buffer binding metadata needed to prove why
those indices were selected. That issue remains open. Different driver
progress can reflect optional features or invalid-descriptor handling; it is
not proof of a driver defect.

## Host validation

* 576 production-backend SPIR-V modules: all U8/S8/U16/S16 operations,
  32 arithmetic/storage/aliasing profiles, SPIR-V 1.0/1.3/1.4, mixed-width
  writable and read-only programs, two buffers, offsets 0..7.
* spirv-val plus structural capability/access checks and an interpreter of
  generated integer/memory instructions. Neighbor preservation, truncation,
  signed reads, dynamic offsets and injected concurrent neighbor updates
  forcing CAS retries are checked. This is deterministic execution, not a
  real GPU contention benchmark.
* 17 storage-image modules, including five extended formats for both image
  kinds, ordinary controls and a negative typeless-write feature case.
* Negative controls on the original compiler: 288/384 single-width modules
  fail validation; among 96 validating modules, 48 incorrectly use native
  storage when storage support is absent. Ten original extended-format
  modules fail validation while six ordinary controls pass.
* Strato tests compile actual feature-chain, TraitManager and profile source:
  64 feature combinations and 3,840 SSBO binding-range cases under UBSan.
* Sampler pool rebinding/cache/lifetime/linked-index/bounds regressions pass;
  the old source fails the stale-index regression.
* Scheduler's 22 host scenarios and relevant DRAW_TEXTURE (16), submission
  (26), and conditional-rendering (20) regressions pass.

Reproduction commands are in tests/gpu_shader_features, tests/gpu_samplers and
in the compiler's tests/storage_subword and tests/storage_images directories.

## Remaining limits and device validation

No APK or MK11 execution on the Moto G100 was performed here. Host validation
cannot prove that the game reaches the menu or gameplay, resolves the descriptor
crash, or preserves frame time on real hardware. No performance improvement is
claimed. CAS is lock-free under progress, not a per-invocation wait-free bound.

Raw global 8/16-bit memory operations remain unimplemented, distinct from the
normal global-to-storage lowering. Uniform-buffer subword feature gating is a
pre-existing separate limitation outside these SSBO changes. Aliasing between
separate read-only and writable shader modules still requires caller
synchronization; fixtures do not prove cross-stage disjoint-byte alias safety.
Unsupported actual storage image/texel-buffer formats require further format
support work; declaring a SPIR-V capability alone cannot supply that support.

The dependent Strato PR builds exactly one ARM64 DevReldebug APK, combining
#362 and the new compiler pin. Host gates precede Android assembly. Gradle
cache and CCache are retained, no clean is added, and other PRs retain their
normal APK variants. Actions is not monitored after publication. Test this
APK on MK11 v1.0.0 with the same driver/settings first; any remaining crash
must be evaluated from the new log without discarding scheduler progress.
