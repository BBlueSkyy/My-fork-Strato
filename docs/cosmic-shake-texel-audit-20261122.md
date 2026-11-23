# Cosmic Shake — continuation on current master, 22 November 2026

## Result and boundary

A sampled texel-buffer numeric-type violation remains in master
`8bcaee0be950b44234cdd2767a11578c74db2602`, pinned compiler
`9cdd405f861229420a57f79832bf68b65c219fe6`. The type correction previously proposed
in unmerged [PR #423](https://github.com/BBlueSkyy/My-fork-Strato/pull/423) is now
adapted to the current metadata/specialization architecture and validated in
reduced production-backend, Vulkan and cache tests. This is a continuation of
that candidate, not a claim of a newly discovered explanation for the game.

**The cause of the complete visual corruption is still unconfirmed.** No game was
executed here. Neither NaN elimination, finite HDR correctness, multiple-boot
stability nor Android FPS/frametime has been established. The next experiment is
the isolated numeric-type correction on Moto G100. No merge is authorized.

Published compiler dependency: `9cbc37dc943de4fae527036c15352e43921e8b8d`,
branch `fix/texel-buffer-numeric-type-20261122`.

## Reused checkpoints

- PR #375's bounded capture and shader/material/cubemap work: draws 19–21 can
  increase finite HDR values drastically. Earlier first-draw capture could start
  without NaNs and finish with a few; later draws amplify corruption.
- PR #378: legal fetch-offset folding was tested without improving the scene.
  That negative visual result is retained. Current compiler folding is preserved.
- PR #380: the SHF low-source-register decoding correction is already integrated;
  do not revert it or repeat its investigation.
- PR #421 / branch `diag/spongebob-overlap-publication-20261102`:
  SBMINI1 measured 140/273 NaN pixels after an entire render pass in two boots.
  SBMINI2 measured different shader/draw identities with zero ROI changes; its
  artificial pass splitting may alter timing. It cannot be treated as an equal
  good/bad pair or evidence against corruption elsewhere.
  SBMINI3 preserves natural pass boundaries; previous captures selected
  composition/fade passes with already colored inputs and approximately 0.6 or
  158/255 scaling. Candidate writer history and short image-buffer ranges do not
  prove which operation wrote the corrupt pixels.
- PR #423 head `b1c69e0749526ba61c0bf6ce1026c7989cab3222`, compiler
  `1c4aff9394adb8f7b80b053bd37e25b5b51ec478`: earlier Float/UINT/SINT proposal.
  Its description explicitly lacks Vulkan/device validation and has no PR
  discussion recording a graphical result. It is not proof of a visual fix or
  proof that this hypothesis was rejected on-device.
- PR #400 is not a demonstrated Cosmic Shake fix. PR #460's pending overlap
  receipt is not proof of the cause. Their branches remain unchanged.
- Existing reports read: frame638 diagnosis, draw7 SPIR-V analysis, stage-2 texel
  audit, PCHE handle audit, cross-compiler analysis and November 5 RenderDoc trace.

The frame638 evidence remains: 112,186 NaN pixels before event1695, 113,451 after
(+1,265), integer R16/R32 views at bindings17/18 accessed by float buffer images.
Another capture has zero NaNs in 12 snapshots but 41,056 pixels above1000 in the
first snapshot. These are different symptoms and captures. Neither observation
locates a common first corrupting draw.

## Pinned reference comparison

Kenji-NX source was accessible by git in this session. Inspected
`projects/Kenji-NX` master `a6a641ada215837297e7170c91c6b4e8275574c6` and Android
branch `libryujinx_bionic` `280c427c550c5fe3bcc46b378e06a9b90a7e6b09`.
Ryubing reference: `69f2c0942452637b7be6965efea390f1868b8d9c`.
These are source checkouts; the exact APK revision used by the user is unknown.

| Area | Strato master | Kenji-NX / Ryubing inspected code | Conclusion |
| --- | --- | --- | --- |
| Sampled buffer image type | One Float image type shared by all buffers | `Declarations.DeclareSamplers` also uses TypeFP32 for Buffer | No evidence that universal UINT explains Kenji's result. A shared declaration is not proof of valid runtime pairings. |
| Numeric view format | Converted directly from TIC | Vulkan TextureBuffer uses `FormatTable.GetFormat(info.Format)` | Neither source comparison alone proves which view is bound by the user's good boot. |
| Buffer storage rebinding | Resolve CachedMappedBufferView, then owner-local view store under recreationMutex | SetBufferTextureStorage and CachedTexture=null; TextureBuffer.SetStorage releases its view when buffer/range/count changes | Different strategy. Strato's key includes backing handle, offset, range and format, separately for each owner; no stale-view fault was demonstrated. |
| Specialization | Current handle-origin metadata tracks type/integer/compare; sampled buffers omit numeric type | Separate texture specialization architecture | Strato needs a numeric dependency in both graphics and compute reuse paths. |
| Handle shifts | #471 preserves secondary composition and snapshots before output dirtiness, but the single-source path still drops its shift; HandleSource also duplicates an unshifted word | Independent handle/texture manager | Both current inconsistencies reproduce in directed tests and are corrected together. |

Reference paths: `src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Declarations.cs`,
`src/Ryujinx.Graphics.Gpu/Image/TextureBindingsManager.cs`,
`src/Ryujinx.Graphics.Vulkan/TextureBuffer.cs`; Android branch has the same sampled
Float declaration and conservative buffer-storage rebind policy. Main/Android
TextureBindingsManager files differ; no assertion of identical complete code.

## Concrete defects and correction

1. **Runtime Vulkan type mismatch:** master emits Image<Float,Buffer> and float4
   OpImageFetch regardless of the TIC-derived VkBufferView. Vulkan requires
   matching numeric categories when accessed:
   [VUID format-07753](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexed.html).
2. **Missing dependency:** changing one sampled buffer between UINT and SINT
   cannot be handled by a boolean integer flag. The new BufferType metadata query
   tracks Float/UINT/SINT with current handle sources and #471 read/retry rules.
3. **Descriptor identity:** buffer deduplication now includes numeric type and
   both handle shifts. A directed TexturePass case proves that two fetches from
   the same CBUF word with different shifts need separate descriptors.

The emitter creates per-descriptor float/unsigned/signed image and result types.
Integer results are bitcast to the existing F32x4 register-bit representation,
not numerically converted to floating point. UINT/SINT Vulkan component expansion
and absent alpha=1 are preserved. RelaxedPrecision is not attached to integer
buffer fetches, which must retain the fetched raw 32-bit values. Float sampled
images, storage-image typing and view formats are not globally rewritten.

4. **Shifted single-source handles:** the current runtime returns the original word
   when has_secondary=false, even with a nonzero shift_left. The compiler's
   canonical HandleSource duplicates that word with secondary_shift_left=0,
   yielding `(word << shift) | word`. For word=3, shift=2, this queries14 but
   binds3 instead of12. The query now duplicates the same shifted source (OR is
   idempotent), and runtime applies the shift. Descriptors without shift fields,
   such as storage images, keep raw handles. Both original directed tests fail
   before correction and pass after it. No occurrence in a corrupt game draw is
   claimed. The #471 absent-secondary fallback, range checks and snapshot timing
   remain intact.

Runtime and disk accessors carry the new query; graphics/compute specialization
rechecks it, including equal-TIC UINT/SINT transitions and return to earlier
variants. Existing integer metadata for storage images is retained independently.
PCHE becomes **v7**, rejecting master v5 and the historical #423 v6, whose payload
layout differs. Using v6 again would risk misreading the earlier experimental
cache. Old shader caches rebuild automatically through the existing version check.

The old #423 implementation was not cherry-picked as a whole: it predates the
current specialization/query architecture and several later compiler fixes. Its
Float/UINT/SINT proposal is retained, while current code and tests are preserved.

## Tests and evidence

| Test | Result |
| --- | --- |
| Original master backend reduced shader | Three accesses emitted as Float buffer fetches; saved baseline SPIR-V passes static validation, demonstrating static validation alone is insufficient |
| All-Float pairing with UINT16/SINT16 views | Exactly two format-07753 validation errors; invalid commands never submitted |
| Production corrected backend | ASan/UBSan; compute and fragment SPIR-V pass Vulkan1.1 validation, matching Float/UINT/SINT image/fetch types; integer precision preserved |
| Executed corrected compute SPIR-V | 36 exact output words across Float32, UINT16/SINT16, UINT32/SINT32, UNORM16, UINT8/SINT8; zero validation errors |
| Production TexturePass | Bound/bindless Float/UINT/SINT metadata; distinct shifted descriptors; shifted single-source handle=12 rather than14; existing 8-target and split-CBUF cases pass |
| Production graphics/compute variant lookup | 129 checks, including Float->UINT->SINT and return/reuse, zero failures |
| Pending Vulkan variant lifetime | 130 checks; 10 pipelines retained while an old fence is pending, zero validation errors |
| Existing TIC/binding regression | 1,186 checks, zero failures under ASan/UBSan; single-source runtime shift test fails on baseline and passes after correction |
| Production serialization / version check | Float/UINT/SINT round trips; existing tables preserved; Reset clears metadata; v5/v6 rejected |
| Production view key/store with host facade | Backing, owner, format, range and offset changes select distinct views; stable key reuses view |
| Preload, query callbacks | Pass; all five query kinds, OR/shifts, source origins and absent/bound secondary behavior preserved |
| Sampled layout / alias regression | 20 upload/readback oracles, mip/array, block spacing and alias paths pass under ASan/UBSan |

Commands are in `tests/gpu_texel_numeric/README.md`. Retained logs are in
`docs/checkpoints/texel-numeric-20261122/`. Tests use extracted production bodies
with host facades where full Android classes are unavailable; the Vulkan compute
readback executes the actual emitted module. No full Android compilation or
complete repository test-suite claim is made here. Leak detection is disabled
because the host cannot support LSan; this is not a leak-freedom certificate.

## Open hypotheses and device experiment

Free Guest changes guest residency, so its effect is relevant, but it does not
establish a stale readback, missing barrier or invalid overlap as the cause of
this scene. No new fault in memoryFreed restoration, HDR initialization, compressed
uploads or overlapping-resource sequencing was demonstrated by this continuation.
Do not clamp HDR, replace nonfinite values, zero resources, add global waits or
force a memory policy. Full-scene NaNs and high finite HDR remain open.

No additional Mini-RenderDoc is bundled with this isolated correction. First
compare this isolated texel-type/handle-consistency correction on-device. This avoids accumulating another
timing-affecting capture change in the same APK.

1. Install the new ARM64 Release once Actions succeeds. First launch rebuilds
   incompatible pipeline metadata. Keep one Turnip version, game/save and
   configuration; start with Free Guest OFF.
2. Fully stop the emulator between boots. Compare the same title scene and
   gameplay point across **at least 6 boots**, since Eden failed only on boot6.
   Record correct/corrupt boots, screenshots and complete logs. Keep cache policy
   identical after the initial rebuild; distinguish cold compilation from hot FPS.
3. If needed, repeat a separate, fixed batch with Free Guest ON; do not alternate
   settings within the first batch. Check stability and FPS/frametime as well as
   colors, exposure and granularity. A single good boot cannot confirm a fix.
4. If corruption remains, the next capture hypothesis is whether equivalent
   early HDR passes already receive different texel/CBUF/material inputs or first
   produce different output. Reuse SBMINI3's natural-pass writer identities;
   filter hashes 636E6A47E46F6021, 7161B14045F710B5, EA18B70958415FF3 and
   1B8CB95A440E18C1 as applicable, not frame638's process-local Vulkan handles.
   Capture a matched good/bad producer pass with bounded input/output HDR stats,
   actual texel-view formats/ranges and GPU-visible relevant bytes. Start at the
   earliest HDR producer, not the already-corrupt event1695. Preserve natural
   pass semantics and mark missing provenance explicitly. Only matched content,
   shader and state evidence can justify narrowing to a draw/instruction.

That follow-up is not a completed capture result. The old captured shader data
lack the current draw's complete runtime buffer contents and do not support a
faithful full shader replay with invented numeric metadata.
