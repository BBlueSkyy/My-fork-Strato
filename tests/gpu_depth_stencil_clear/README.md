# Depth/stencil load-operation regression

Run `python3 tests/gpu_depth_stencil_clear/run.py`. Set
`VULKAN_HEADERS_INCLUDE_DIR` to an installed Vulkan-Headers include directory
when the repository's Vulkan-Headers submodule is unavailable.

The harness extracts and compiles the production
`RenderPassNode::ClearDepthStencilAttachment` and `ClearColorAttachment` methods with real Vulkan types and
UBSan. It checks depth-only, stencil-only, and combined attachment formats,
preserved store operations, clear values, repeated clears, and fallback after
earlier attachment use or independently configured aspect load operations.
Before the fix, the stencil-only case fails because `stencilLoadOp` stays Load.
Mixed color/depth clears in both index and invocation orders, and descending
color attachment clears, verify that clearing a lower attachment index retains
every higher attachment's clear value. The regression fails before the grow-only
resize fix because the higher value is removed while its load operation stays Clear.

This is a host regression test, not GPU execution or a Maxwell register-level
test. Maxwell's existing partial-aspect clear dispatch remains outside this
method: it uses `vkCmdClearAttachments` with the requested aspect mask.
