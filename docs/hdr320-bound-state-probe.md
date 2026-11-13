# HDR320 bound-state probe (observation only)

Question: can fragment shader `42016CE58CA1A259`, while drawing 320×180, read
parent mip1 at the same time as it writes that mip? Offline shader decoding
confirmed four TEXS 2D implicit-LOD samples, but PCHE does not save the dynamic
view ranges or sampler state. This probe fills only that gap.

Baseline rendering behavior is PR451 `7ca4af5a`. The new Draft targets that
branch so its diff contains only observation, tests and separate APK packaging.
PR451, master and other Drafts are unchanged. Local checkpoint `02d4b856` and
its prior probes, experiments and audit remain preserved in their original
branch. Old multi-shader and TextureManager routing/allocation logs are not
included in this publication.

## Observation

Only direct draws with this fragment hash and scissor extent 320×180 emit logs.
The existing capture identifies the pass as a direct indexed draw of 3 vertices.
Indirect/inline-index paths are not instrumented. At most 12 distinct signatures
are emitted per Maxwell3D instance, with at most 8 sampled bindings each. Repeats
of the same bound images/views/samplers/ranges/TIC do not consume the limit, so
unchanged menu frames do not exhaust it before a scene transition. The signature
uses a bounded 64-bit diagnostic hash, so deduplication has a theoretical collision
risk. `cap=true` identifies the last admitted state.

- `HDR320_DRAW`: event correlation, shader, channel sequence, scissor, binding cap.
- `HDR320_SAMPLE`: actual bound native image/view, generation, captured view mip
  and layer range, bindless handle, TIC index, raw TSC index and effective TSC
  index (including ViaHeaderBinding), eight raw TIC words, cbuf and array element.
- `HDR320_SAMPLER`: eight raw TSC words from the entry owning the bound VkSampler;
  guest LOD/bias/filters and actual VkSamplerCreateInfo LOD/bias/filters,
  anisotropy and normalized-coordinate mode.
- `HDR320_TARGET`: native target image, generation, backing extent/levels,
  attachment mip/layer range and effective target dimensions.

Binding metadata is copied at the existing full/quick descriptor writes and is
retained alongside active sampled images when no update occurs. No fresh TIC
resolution, bindless-handle read, sampler pool refresh or Vulkan GetView call is
performed by the draw logger. The TIC snapshot is from the cache that produced
the binding. Sampler creation parameters are copied **after** successful native
sampler creation; only the copy's pNext is cleared to avoid dangling pointers.
No pNext-only extension policy is reconstructed from that cleared copy.
Unknown/null metadata is explicitly marked and must not be treated as evidence.

Source ranges/images are recorded at descriptor construction, not reconstructed
from the Texture's potentially promoted backing at logging time. This preserves
evidence of a bound older descriptor. Targets are observed from the draw's
existing attachments. There are no new GPU commands, barriers, readbacks, waits,
submissions, texture routing rules, or shader/sampler/blending changes.
Diagnostic costs are bounded CPU metadata copies/cache storage and log output;
this is not a performance-validation build. Binding snapshots stop after the
12-state cap. The small creation-info copy stays with each cached sampler.

## Interpretation

Compare SAMPLE and TARGET native **image** handles and their overlapping mip and
layer ranges. Equal Texture pointers alone do not establish feedback. Consult
the effective sampler, not only the guest TSC. For Vulkan enum values, min/mag
filter 0=nearest and 1=linear; mipmap mode 0=nearest and 1=linear. Guest min/mag
values are 1=nearest/2=linear; guest mip filter 1=none/2=nearest/3=linear.
A fractional positive LOD may blend mip1 with linear mip filtering; a small
positive clamp does not universally exclude mip1.

The probe establishes whether the bound state permits access to the written
mip, not the derivative-selected LOD of every fragment. It neither samples
implicit derivatives itself nor changes LOD to test the hypothesis. It also
makes no claim that PR451 expansion executed; that earlier independent question
is deliberately not reinstrumented here.

## Validation and device use

Host test: `CXX=clang++ python3 tests/hdr320_probe/run.py`. It compiles actual
production observation bodies and all diagnostic format strings against host
state facades and real Vulkan-Hpp types, with UBSan. Covers retained snapshots,
partial slots, TIC mutation, backing change, effective sampler metadata,
ViaHeaderBinding, nonmatching draw rejection, duplicate suppression and cap.
It is not a whole Android build or device/Vulkan rendering test.

The PR workflow retains full history for prepare.py, builds ARM64 DevRelease,
verifies nonempty APK/AArch64 native ELF libraries and uploads
`strato-pr<PR>-hdr320-probe-arm64-release.apk` with missing-artifact errors enabled.
App: **Strato HDR320 Probe**, package `org.stratoemu.strato.hdr320probe`.
Install separately, select the same Turnip driver with Free Guest Texture Memory
OFF, reproduce Mario Kart Stadium and share the normal emulation.log containing
`HDR320_` records. No mini RenderDoc recapture is needed for these fields.
Build execution is not monitored, following the user's standing preference;
workflow completion/APK existence are not asserted before execution.
