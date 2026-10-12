# RE5 — final targeted 128x128 cube probe (PR #337)

This is a **diagnostic only** follow-up to the paired captures, not a graphical
fix. In the supplied captures, FS combined-image-sampler binding 21 describes a
128x128 RGBA8 cubemap with eight requested mips in both states. The tracked
guest texture has eight levels in the bad state, one level in the good state.
The previous multiplier scalar values were nonzero and equal. This does not
prove that a cube sample causes the black geometry.

## Added observations

- Existing `draw_bindings_###.txt` records the texture's tracked
  `host_allocated_mips`, layers, view ranges and per-range fit flags.
  These are Texture allocation metadata; view creation backing is compared to
  the current VkImage. Do not infer GPU shader execution from this.
- For only FS binding 21, Cube, width and height 128, records TSC indices,
  guest mip filter, fixed-point min/max clamps, the clamping values used for
  Vulkan sampler creation, mip bias, and filter mode. It does not create or
  refresh a sampler.
- For the same descriptor records `cbuf 12 + 0x4` as raw bits and float
  **from the unsynchronized guest mirror**. A GPU-dirty/direct-backed guest
  buffer can differ from the actual uniform buffer contents read by the
  shader, and the byte has not yet been proven to be a dynamic LOD instruction.
- On the **first qualifying draw per capture** (across executors), snapshots
  the sampled texture's *current backing* at the natural render-pass end,
  using the existing image-to-staging readback helper and fence callback. The
  strict gate requires RGBA8, 128x128x1, six layers, 1..8 tracked mips,
  `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, General layout, matching view-create and
  current backings, and no more than 2 MiB. A mismatch skips the copy rather
  than silently reading a different VkImage.

A successful copy saves `cubemap_binding21_000.raw` (full backing, all
tracked mips and six layers) and `cubemap_binding21_000.txt` (metadata,
mip offsets, byte counts, postpass timing and copy status). Raw bytes are in
**mip-major order**, each mip containing all six layers. This is a
post-render-pass observation, not the contents at an exact shader load. The
normal `draw_bindings_*.txt`, `scalar_*.txt` and `blit_history.txt` are
still collected unchanged.

No texture selection, VkImageView creation, shader, constant buffer, sampler,
guest state or Texman behavior is modified. The bounded image readback does
add a transfer copy and the helper's existing transfer/host visibility
dependencies, but does not add waits or split passes. Performance may be
affected around that single readback.

## Pairwise comparison

1. Install the Android APK from this PR after the CI build has completed.
2. Restart Strato, load the problematic RE5 chapter and capture bad state.
   Rename `gpu_capture/re5_scene/ARM_CAPTURE.rename_this_file_when_ready`
   to `ARM_CAPTURE`, return to gameplay and wait at least 10–15 s, plus any
   late readback callbacks.
3. In the same session, load chapter 4-1 and return to the same scene.
   Capture good state with the same procedure.
4. Collect both full capture folders and logs, especially the two
   `cubemap_binding21_000.raw/.txt` pairs and the sampler/LOD lines from
   `draw_bindings_*.txt`.

If the cube files are missing, inspect the view status, `host_allocated_mips`,
`view_backing_matches_current`, and `gpu_events.txt` budgets. The capture
may have ended before a sampled cube, or the safety gate may have rejected it.
**Do not automatically add more probes** in response to a missing file.

Compare image bytes by mip and layer, never by raw host address or texture ID
across executions. Equal LOD mirror values do not establish equal GPU values.

## Local validation

`g++ -std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -I app/src/main/cpp/skyline tests/gpu/re5_cubemap_probe_tests.cpp -o /tmp/re5_cubemap_probe_tests && ASAN_OPTIONS=detect_leaks=0 /tmp/re5_cubemap_probe_tests`

The pure gate/range unit test passed locally. Android integration and
on-device Vulkan correctness must still be validated by the PR build/device.
Maintain this PR as Draft. No merge or master modifications.
