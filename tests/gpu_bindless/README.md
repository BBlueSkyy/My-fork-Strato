# Bindless and Vulkan descriptor regressions

Run `python3 tests/gpu_bindless/run.py` with recursive submodules checked out.
Set `SHADER_COMPILER_ROOT` for a standalone compiler checkout. The fixture compiles
actual handle evaluation/binding and graphics/compute layout/write lambdas with
Vulkan-Hpp and UBSan. It tests samplerless access, invalid required samplers, null
images, mixed descriptor types and numeric variant matching. No driver calls.
