# Metroid Prime Remastered / nvhost-as-gpu audit

Turnip 24.0.99 log: 1,115,166 unmapped-texture warnings concentrated on six GPU addresses. The log ends without a native crash trace. This is not proof that the mapping defects below caused the hang.

Static findings:
- UnmapBuffer left a stale fixed-mapping reference in the owning AllocSpace record. A later FreeSpace would try to unmap the same mapping again and could fail with InvalidArgument.
- MapBufferEx remap checked length but not offset plus length, permitting out-of-bounds subranges.

These generic issues are fixed on this branch. No title-specific workaround, Texman changes, or synthetic mappings.

Validation: bash tests/nvhost_as_gpu/run.sh (C++20, -Werror, UBSan): passed locally. Android build and gameplay remain unverified.

Next: determine if the six TIC addresses were never mapped, were freed prematurely, or have incorrect texture sizes; separately capture the native crash or thread stacks during a freeze.