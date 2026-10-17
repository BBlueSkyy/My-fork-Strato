# Constant buffer selector regressions

Run from the repository root:

```sh
python3 tests/gpu_constant_buffers/run.py
```

A C++20 compiler, Boost headers and AddressSanitizer/UndefinedBehaviorSanitizer are required. Set `BOOST_INCLUDE_DIR` if Boost is outside the system include path. Optional arguments select `hint`, `holes`, `sparse`, `remap`, `load`, or `read`.

The harness compiles the actual production `ConstantBufferSelectorState` class and all its methods, the actual `ConstantBuffers` class and complete methods (including `Load` and `Bind`), the strict `CachedMappedBufferView` methods, `ConstantBuffer::Read`, dirty-state wrapper, and real `FlatMemoryManager`/`SegmentTable` code. It reuses only the dependency preamble from `tests/gmmu/tests.cpp`. Android/Vulkan buffer allocation, locking and GPU submission transport use small host facades; no production selector algorithm is copied into the tests. The GPU-copy fallback in `Load` compiles but is not executed.

Cases cover the default 64 KiB declaration with a 16 KiB mapping, a hole in the middle, disjoint resident pages before an unmapped tail, explicit sparse zero mappings, same-address smaller/larger remaps, equal-generation address-space changes, complete unmap, offset writes, rejection before locking/writing, repeated access without retranslating optional capacity, and preserved cached-buffer alias preflight. A separate strict-cache check ensures ordinary buffers still reject an unmapped tail. Read checks use a smaller view over a larger backing buffer so the production view boundary must reject the invalid access itself.

For a negative control against an older selector implementation:

```sh
CONSTANT_BUFFER_BASELINE_REF=8291e41c python3 tests/gpu_constant_buffers/run.py hint
CONSTANT_BUFFER_BASELINE_REF=8291e41c python3 tests/gpu_constant_buffers/run.py load
```

The first rejects a valid 16 KiB bind because of the 64 KiB declaration. The second reaches the write transport with an undersized view because it omits the write offset from the required extent. Both are expected to fail.
