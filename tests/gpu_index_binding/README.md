# Index binding cache regression

Run `python3 tests/gpu_index_binding/run.py` from the repository root.
Set `INDEX_BINDING_BASELINE_REF=<commit>` to compile that revision's production
sources. Only an explicitly selected baseline revision without
`ActiveState::InvalidateIndexBuffer()` uses a clearly reported empty host method
so the missing-invalidation regression fails at runtime. The ordinary run
requires the production method to exist.

The runner compiles the actual `IndexBufferState` declaration, constructor,
`Flush`, `Refresh`, `PurgeCaches`, conversion helper and register binding method.
It also compiles the full production dirty-tracking header and, when present,
the actual `ActiveState::InvalidateIndexBuffer()` method. Platform-dependent
includes are replaced by host types. Buffer mapping, allocation, megabuffer
results and Vulkan command recording are facades. The binding sink observes
which production cache paths issue an index bind.

The production `StateUpdateBuilder::HasIndexBufferBinding()` body is also
tested against empty, unrelated, static-index and dynamic-index command lists.
Command headers and record-function identities are facades for this observer;
the traversal and classification algorithm is unchanged production code.

The quad and inline tests simulate the synthetic binding side effect between
guest draws, checking both megabuffer and direct backing, and verify that narrow
invalidation preserves mapped-view caches. The real draw call sites are reviewed
separately; the fixture does not compile or execute the complete draw functions
or their diagnostic mode gates. These tests prove the cache invariant, not the PAC-MAN cause or
on-device Vulkan behavior.
