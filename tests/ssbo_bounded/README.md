# Compute SSBO host propagation regression

```sh
python3 tests/ssbo_bounded/run.py --compiler app/libraries/shader-compiler
```

This compiles the current production bodies of `GetStorageBufferBinding`, `CachedMappedBufferView::Update/PurgeCaches`, and `BufferView::GetBinding` with a deterministic memory/backend facade and UBSan. It also uses the actual compiler `ComputeStorageBufferAccessRange::GetSize` header.

96 combinations cover sized/raw descriptors, readonly/writable buffers, megabuffer/deferred bindings, 4/256-byte alignment, aligned/unaligned pointers and 4/4608/196608-byte inferred extents. Additional cases check unknown 8MiB fallback, shrinking an existing cached view, physical backing versus view range, overlapping aliases, delegate relocation to a successor, unmapped and partially mapped ranges, contiguous split recovery and noncontiguous split refusal.

The extraction is intentional: changing the production routine changes the code under test, without duplicating its implementation. The facade does not model Vulkan execution, page traps, actual BufferManager coalescence, races or fences. Those mechanisms are unchanged by this compiler-only fix. The alias/relocation cases validate the deferred binding contract, not a full concurrent buffer-manager implementation. Hardware validation must still verify Zelda and Marvel gameplay/graphics.
