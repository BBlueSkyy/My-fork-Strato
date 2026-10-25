# Texture resource sequence regression

Run `python3 tests/gpu_texture_sequence/run.py` (C++20, UBSan by default).
Optional groups: `uploads`, `copies`, `failures`. Set `SANITIZE=address,undefined` to also check host memory handling.

The runner compiles the actual production methods without rewriting their bodies. CPU resource and GPU interfaces are host facades, with gated completion for CPU reuse and injected allocation/record/submit failures. These tests prove state/lifetime ordering at those interfaces, not actual Vulkan barriers, shader behavior, Android signal handling or game performance.

Negative control: `TEXTURE_SEQUENCE_BASELINE_REF=9935f133 python3 tests/gpu_texture_sequence/run.py uploads` fails for lost CPU upload authority; group `copies` fails for the untracked source reader. Inline failure metadata is tested with a fresh/discarded recording context: no command-buffer rewind is claimed.
