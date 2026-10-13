# RE5: Fermi2D → TextureManager → Texture views

This audit supplements the scalar diagnostic in PR #337. The developer's hint is a texture requested by the Blit Engine failing Texman. No RE5 root cause has been established and no rendering correction is applied.

## Pinned sources

| Source | Commit |
|---|---|
| Fork master pinned for this audit (verified with git ls-remote) | `be16463d5d3cb30f9ad99bc0466b2e8b0a0a9211` |
| PR #337 base / original audit | `140dae65115cddc1289ab222b27faef18f66d83d` |
| Fork Texman v2 #244 | `0a4a6c94868bb855f32c880789260730da4104b2` |
| Eden | `6352612425ae0765802eaa8c5904ff652b97222a` |
| Ryujinx mirror | `793ca017ff4f6e48b798a62d2486405bfc42ef39` |

The developer's upstream Texman rewrite is not assumed to be the fork's PR #244. A difference in #244 does not prove a fix in the developer's implementation.

**Confirmed device result supplied during this audit:** RE5 was tested on PR #244 and the black characters/ground persisted. The corrected mip lookup in that branch must not be presented as an RE5 solution. This result lowers the priority of an isolated mip-lookup explanation; it does not exclude shared Fermi/view/alias paths or establish equivalence with the developer's upstream rewrite. The exact tested APK commit was not supplied, so this report does not infer an instruction-level equivalence from the PR number.

## Strongest master divergence: an ordinary nonzero mip is missed

`gpu/interconnect/fermi_2d.cpp:135–146` constructs one-mip 2D source/destination requests from the Fermi surface addresses and calls `TextureManager::FindOrCreate`. The address can legitimately identify a mip of an existing larger image.

The loop at `gpu/texture_manager.cpp:141–160` in the #337 base (146–165 in current master) computes `memOffset`, then advances **both** `level` and `levelMemOffset` only inside:

```cpp
if (layerMemOffset + levelMemOffset == memOffset) {
    if (mipLevel.blockLinearSize == guestTexture.CalculateLayerSize()) {
        matched = true;
        matchLayer = layer;
        matchLevel = level;
        break;
    }
    level++;
    levelMemOffset += mipLevel.blockLinearSize;
}
```

For a request starting at a nonzero mip, the first offset differs, so the cursor never advances to that mip. The request can fall through to a newly allocated representation instead of a view of the cached backing. Ordinary partial-overlap candidates are not added to the `matches` list that is synchronized at lines 316–317; the separate 3D slice reconstruction has additional handling and does not establish generic 2D mip coherence.

Reproduction executed with the **exact loop extracted from the pinned master**, compiled under ASan/UBSan; the extracted loop is byte-identical in current master `be16463d`. It was compared with the production #244 `LocateSubresource` helper:

| Input | Value |
|---|---|
| Mip byte sizes | 65536, 16384, 4096 |
| Layer stride / count | 86016 / 1 |
| Requested byte origin / size | 65536 / 16384 |
| Master exact-loop result | no match |
| #244 `LocateSubresource` result | layer 0, mip 1, offsetWithinMip 0 |

#244 `texture_manager.cpp:337–354` uses `LocateSubresource`; `texture/guest_range.h:161–165` subtracts every preceding mip size unconditionally. Its guest-range and resource-layout host tests also passed.

**Confirmed:** a master lookup defect, reachable by a nonzero-mip Fermi request. **Not captured:** whether RE5 made that request, which image was authoritative, whether fallback bytes were stale, or which later material draw sampled it. The user's persistent bug on #244 is contrary evidence to treating this difference alone as sufficient. Applying a RE5 correction from this finding alone would still be speculative.

## Other concrete comparisons

1. **Mapping offsets.** Master line 141 subtracts a pointer from the first physical mapping to compute a logical image offset. A request in a later noncontiguous mapping cannot be located this way. #244 validates ordered mappings and resolves a contained logical offset (`texture_manager.cpp:259`; `texture/guest_range.h:38–50,95–110`). Actual translated spans are missing from the captures.
2. **Depth-copy aliases.** Eden's `texture_cache.h:1760–1798` allows matching-format point blits to search relaxed formats and performs paired `DeduceBlitImages` (`util.cpp:1277–1296`). Ryujinx permits Fermi D24-family/BGRA aliases in `TextureCompatibility.cs:245–249`, enables depth search in `TwodClass.cs:304–306`, then deduces destination depth format at 336–338. Strato `FormatBase::IsCompatible` (`texture/texture.h:122–129`) rejects D24/color sharing; its explicit cross-aspect exception is D32Sfloat→R32Sfloat. The captured D24/S8 attachment alone is not evidence of a Fermi depth alias.
3. **#244 scaling warning.** Both trees set Fermi source extent to derivative × destination extent at `fermi_2d.cpp:155–156`. #244 passes `duDx,dvDy` again at 167 and multiplies them again in `shaders/helper_shaders.cpp:255`. Its UV span is derivative² × destination extent / source extent; master's is derivative × destination extent / source extent. This changes non-unit scaling and loses a negative derivative's reversal. It does not explain a problem in master and must not be imported as a fix.
4. **Returned views in the capture-era base.** The #337 base and #244 `Texture::GetView` (`texture.cpp:893–919`) retain host-format substitution, cross-aspect fallback and RGBA/BGRA swizzle handling. `TextureView::GetView` (`texture.cpp:83–99`) keys the actual Vulkan view on type, format, mapping and full subresource range. The blit helper uses that view in its descriptor (`shaders/helper_shaders.cpp:266–280`). Captured backing dimensions/format do not identify those view properties.

## Current master has additional changes; do not confuse them with the capture-era base

The repository advanced while this investigation was running. A fresh `git ls-remote` identified `be16463d`; it was fetched and inspected in a separate detached worktree. The diagnostic PR's existing history is preserved, and master is not modified.

Relative to `140dae65`, current master rejects equal-byte-size/full aliases with different logical dimensions, normalizes Fermi blits against the actual returned view's mip dimensions, and enforces admitted view formats. Foreign-format requests can promote a backing through a dependency-preserving GPU copy, invalidate old view generations, and retry preparation via `RunWithTextureBackingRetry`. These changes must not be reintroduced as new RE5 fixes. They do **not** change the faulty nonzero-mip loop or the cross-mapping pointer subtraction identified above.

The original snapshots do not record the data needed to prove a promotion, retry, nonzero-mip lookup or alias mismatch. They also cannot establish the behavior of a later APK built from current master. The scalar diagnostic continues to clear descriptor references on submission/flush and pipeline invalidation; it does not interfere with a restart after backing promotion.

## What the supplied captures establish

Both four-frame captures contain 145 sampled `SCENE_DRAW` records; bad has 21 post-pass images, good has 22. Searching the available capture/log/analysis text found no Fermi operation, Texman lookup decision, GetView decision, mip/layer selection or failure linked to a blit. The emulation log's errors are audio latency errors, not texture exceptions. Absence of an exception does not prove correct cache reuse.

The scene draws identify an upstream 1280×720 RGBA8 render target sampled by a material draw into the downstream 1280×720 RGBA8 target, sharing D24/S8. At pass02, downstream mapping `0x1870A90000` is common to both captures. A matching material draw ordinal 224 has count 571, VS `F8A73DBC9B2E4E05` and FS `F3805B306B15F89A`. The upstream packed-channel image exists in both; black RGB is already present in the downstream pass, before presentation/postprocessing. This does not identify the producer of the 8×8 or 128×128 sampled inputs.

The phase-2 image filter only captures 1280×720, single-level, single-layer color outputs paired with depth. A small image, mip/array view or earlier chapter-loading blit may be entirely outside that coverage. Captures collected after entering gameplay cannot recover its initialization history.

## Minimum next experiment for the blit hypothesis

First capture **request and selection metadata from chapter loading**, not another identical four-frame color dump:

- Source/destination Fermi addresses, request format/dimensions, stride or block height/depth, rectangle extents, `duDx/dvDy`, filter and origin.
- Complete translated mapping sequence and candidate backing's logical subresource offsets.
- Lookup route (existing mip/layer/full view versus new representation), selected backing identity and returned view type/format/aspect/mip/layer/swizzle.
- One later material binding with the same guest range/backing identity, to join producer and consumer.

Only after that join is established are before/after bytes of **that one subresource** needed to distinguish bad alias selection from stale contents or visibility. Prioritize the shared partial-overlap/format-view/coherence paths in master and the tested #244. A request at mip 1 falling through while the parent has current GPU data would establish that defect's involvement, but still would not prove it is sufficient to resolve the game given the #244 result. If all relevant requests are mip 0, that specific hypothesis is excluded for those requests.

The already authorized scalar probe is preserved and published separately from these findings. It measures dynamic multiplier bytes missing from the original captures; it **does not close the blit-history gap** and does not add Fermi rendering changes.

## Follow-up instrumentation in the same PR

Phase 4 now implements the bounded chapter-loading journal and later draw-binding link described above; see `re5-scene-mini-renderdoc.md`. It preserves the published scalar and image probes and does not apply the mip-loop change or any other rendering correction. The failed test on #244 remains contrary evidence to calling mip 1 a proven RE5 solution.

The fork advanced again to `c7cb28d822c52437ed83e9f86ed15c3a3b77c6e7` during final validation. It was fetched read-only for an integration check; the source pins and line references above describe the earlier audit, not a new claim about every change in that head. Publishing this diagnostic does not modify master. The new good/bad captures have not yet been collected, so no requested Fermi resource has been proved to be the cause.
