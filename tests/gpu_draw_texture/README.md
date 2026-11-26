# DRAW_TEXTURE host regression checks

Run from the repository root:

```sh
python3 tests/gpu_draw_texture/run.py
```

Requires Python 3 and g++ with C++20/UBSan support. The runner extracts and
compiles the actual production `Maxwell3D::DrawTexture`, programmable sampler
index-selection overload, filter converters, and DRAW_TEXTURE register struct.
It does not copy the rectangle algorithm into the test.

Host interfaces replace GPU allocation, descriptor creation and recording. They
capture the selected source, target, sampler, rectangles, dimensions and cycle
references. The explicit sampler lookup boundary is a fake; the production
Vulkan sampler cache/construction and helper shader recording are not compiled.
These tests establish host behavior, not Android build success or rendered pixels.

The tests cover explicit versus linked shader sampler lookup, the captured final
rectangle, negative extents, missing/feedback/empty targets, view mip dimensions,
source retention across cache eviction, SurfaceClip/scissor0 and origin transforms,
and defined reserved filter conversion. No game files are required. Resource IDs
and dimensions in the fixture describe an ordinary texture transport scenario.

Failures observed before correction included the wrong TSC, parent-image UV
normalization, missing source cycle ownership, ignored clips, bit-4 rectangle
translation and a UBSan no-return error for a zero filter.
