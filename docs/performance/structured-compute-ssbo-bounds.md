# General bounded Compute SSBO inference after PR #294

This production candidate continues investigation #404. It does not import that PR's instrumentation or the certified `bf3406df` experiment. Strato base: master `b8593d8b2c2e9742a4f1f9edfaa935f653cd3663`. Compiler base: its existing gitlink `c10731c2d5ab00dbc115e651edbb88e221dcf9cc`, preserving modern descriptor recognition and subsequent compiler fixes.

## Established result and remaining distinction

The user-confirmed A–B–A test with the same modern compiler recovered Zelda performance when the effective SSBO0 extent was reduced from 8MiB to the separately proved 4,608-byte extent. Tail 18 five-second windows in uploaded A1/B/A2 logs give 17.80 / 29.42 / 18.07 presented frames/s. Zelda graphics were reported normal in B. This experiment need not be repeated.

The supplied summary said the target physical backing remained 8,470,528 bytes in B. The actual B target rows instead report `backing_max` and `prepared_backing_max` at most 86,016 bytes, versus 8,470,528 in A. Therefore the sizing intervention is causal in that test, but the experiment does not separate driver descriptor-range cost from the smaller physical-buffer consequences. The compiler fix targets the proven interval without changing BufferManager, synchronization or megabuffer heuristics.

## Mechanism

The affine metadata pass could not handle induction Phis, split U32 multiplication, or a conditional Phi whose alternate input is unreachable at the memory access. That left a raw Compute pointer's extent unknown and made the existing host choose an up-to-8MiB fallback.

The compiler now retains the original affine path and, only for unknown unsized Compute descriptors, attempts a bounded structured proof:

1. Reconstruct emitted ASL control flow; validate exact IR edges and Phi predecessors, reachability and cycles.
2. Prove literal-entry, positive-step loop recurrences using dominance, a unique latch/backedge, the exact next-counter continuation comparison, and a no-overflow check. Equal self-recurrences with matching entry/start/step are lockstep.
3. Intersect must boolean facts at joins and combine each Phi predecessor's facts with the memory access context. Exclude only contradictory or unreachable incoming paths. Both Repeat edges discard facts, since backend safety-counter exits do not imply that the guest continuation condition was false.
4. Conservatively union every surviving access, including its complete scalar/vector width. Distinguish upper-bound scalars from exact constants: an upper bound cannot multiply the symbolic pointer base. Unsupported expressions, cyclic Phis, unknown bounds, wraparound and exhausted budgets stay unknown.

For the captured Maxwell input, the proved index spans 0..31, the stride is 144, and the final 16-byte access is at record offset 128. The general calculation is `31*144 + 128 + 16 = 4608`. Conditional-flow proof removes the incompatible high-word Phi input only at accesses where its condition is contradictory. No shader hash, game ID, fixed CBUF offset or 4,608-byte cap is present in production code.

The proof changes metadata only. Raw pointer detection, adjacent-high-word validation, `has_size`, NVN descriptors, existing affine Compute ranges and conservative fallback remain available. Atomics retain unknown-size handling. No memory-ordering, page-trap, dirty-state, alias/coalescence, texture or render-pass code changes.

## Host propagation

`Pipeline::SyncDescriptors()` already passes `desc.access_range.GetSize(groups, dimensions)` into `GetStorageBufferBinding()`. That routine adds pointer alignment padding and respects the Vulkan device maximum. `CachedMappedBufferView::Update()` resolves/clamps the requested mapping using existing rules; `BufferView::GetBinding()` retains the logical view size while following a relocated backing delegate. The compiler gitlink is the only production integration required. A small extent is not the physical backing size and is not a claim that only that many bytes were written by the guest.

## Local validation

* RED/GREEN: the actual pass initially failed the 4,608-byte bounded-loop regression; the implemented proof passes. A separate pointer-times-induction counterexample failed before the exact-constant guard and passes conservatively afterward.
* Synthetic actual-pass tests passed under AddressSanitizer and UndefinedBehaviorSanitizer (LeakSanitizer disabled under host restrictions). Includes exhaustive 27 loop combinations; unknown thresholds/initial values/recurrences; unsigned/signed wrap checks; multiplication overflow; vector read/write widths; unknown atomics; correlated/unrelated Phi and unions; graph/Phi validation; boolean DAG budgets; nested loops with unknown inner bounds; and raw 8-byte-pointer affine 192KiB ranges.
* Full native compiler + SPIR-V backend Release/O3 replay built from the checked-in harness. Reused the existing 4,912-byte Maxwell capture; no new device capture. The target extent is 4,608; the second raw descriptor remains four bytes; the two sized writable descriptors remain unchanged.
* Both reference and generated SPIR-V pass `spirv-val` for Vulkan 1.1/SPIR-V 1.4. All executable instructions match; the comparer allows only capability order and a constant-ID/declaration permutation, and rejects a deliberate arithmetic mutation. No graphical work was removed by this metadata pass.
* Host tests execute the production binding/mapping/deferred-resolution bodies under UBSan. 96 combinations plus alias/relocation and partial/split mapping cases passed. These use a deterministic facade and do not emulate real Vulkan execution, page traps, concurrent coalescence or fences. Those mechanisms were not modified.
* Independent source safety review found no remaining blocker after the exact-constant guard. Diff whitespace and Python/workflow syntax checks passed.

Compiler tests live in its `tests/structured_ranges`; host integration test: `python3 tests/ssbo_bounded/run.py --compiler app/libraries/shader-compiler`. The existing PR workflow keeps `assembleDevRelease`, ARM64-only app ABI, Java 17 and ccache. It runs the synthetic/host checks before Android assembly and verifies a nonempty APK containing only AArch64 ELF libraries before upload.

## Device acceptance still required

The new general implementation has not been run on the Moto G100. The earlier B result is not a measurement of this build. Validate the already tested Zelda scene with Turnip 24.0.99, same settings/save/thermal conditions; confirm FPS and complete objects, effects, shadows and UI. Also validate Marvel Cosmic Invasion gameplay, including lighting, Compute effects and absence of magenta corruption. Modern classification is preserved, but compatibility is not confirmed without that test.

Unknown patterns intentionally retain the old conservative fallback. Compilation cost is bounded and occurs only for descriptors the fast analysis could not prove; there is no per-dispatch proof/instrumentation or new setting. No merge. Build success/artifact existence must not be claimed until an actual completed run is confirmed; after requesting the build, do not poll its progress.
