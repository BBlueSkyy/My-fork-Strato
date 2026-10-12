# Backing promotion verification

Initialize the pinned Vulkan-Hpp and Boost submodules (including Boost's children).
Use Vulkan C headers matching Vulkan-Hpp's `VK_HEADER_VERSION`; this branch's
pinned Hpp is version 202. Set `VULKAN_HEADERS_INCLUDE_DIR` if those headers live
outside `app/libraries/vkhpp/Vulkan-Headers/include`.

```sh
python3 tests/gpu_framebuffer_cache/run.py
python3 tests/gpu_draw_texture/run.py
python3 tests/gpu/run_global_pointer_tests.py
RUN_VULKAN_VALIDATION=1 bash tests/gpu/run_portrait_blit_tests.sh
```

The last command includes `tests/gpu_backing_promotion/run.py`. It requires
`VK_LAYER_KHRONOS_validation`, an ICD with BC compression, image-format lists and
imageless framebuffer support. Set `VULKAN_LOADER_LIBRARY`, `LD_LIBRARY_PATH`,
`VK_ICD_FILENAMES` and `VK_LAYER_PATH` when using a private Vulkan installation.
The test enables synchronization validation as well as ordinary validation.

Promotion tests compile the actual production function bodies unchanged against
host allocator/scheduler/fence interfaces and real Vulkan resources. They do not
replace the image migration with an imitation. They deliberately prohibit image
move assignment to catch duplicated allocation ownership. Failure paths must
leave backing/generation intact and retain evicted framebuffers for old work.

Readbacks exist only in the test to compare every byte of all mips/layers. They
are not part of the runtime policy. These checks establish host-side Vulkan
legality and migration semantics, not mobile-driver performance or a complete
Android build. Device acceptance is described in
[`docs/selective-mutable-format-audit.md`](../../docs/selective-mutable-format-audit.md).
