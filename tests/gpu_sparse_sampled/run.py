#!/usr/bin/env python3
"""Guard the sampled sparse texture contract against the unsafe PR #472 design."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADDR = (ROOT / "app/src/main/cpp/skyline/common/address_space.h").read_text()
TEXH = (ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.h").read_text()
TEX = (ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.cpp").read_text()
MANAGER = (ROOT / "app/src/main/cpp/skyline/gpu/texture_manager.cpp").read_text()
TICS = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/common/textures.cpp").read_text()

assert "bool IsRangeReadable(VaType virt, VaType size)" in ADDR
assert "bool TryRead(span<u8> destination, VaType virt)" in ADDR
assert "predecessor->extraInfo.sparseMapped" in ADDR
assert "std::memset(output, 0" in ADDR
assert "if (predecessor->phys == nullptr)" in ADDR

assert "bool sparseReadOnly" in TEXH
assert "std::function<bool(span<u8>)> sparseRead" in TEXH
assert "if (guest->sparseReadOnly)" in TEX
assert "Sparse backing is not process VMM memory" in TEX
assert "guest->sparseRead(span<u8>{sparseBacking.data(), sparseBacking.size()})" in TEX
assert "dirtyState = DirtyState::CpuDirty; // Conservative correctness" in TEX

sparse_branch = MANAGER[MANAGER.index("if (guestTexture.sparseReadOnly)"):MANAGER.index("/*", MANAGER.index("if (guestTexture.sparseReadOnly)"))]
assert "SetupGuestMappings" not in sparse_branch
assert "aliasReadOnly = true" in sparse_branch
assert "GetView(" in sparse_branch

assert "supportedSparseSampled" in TICS
assert "IsRangeReadable(backingIova, sparseSize)" in TICS
assert "gmmu.TryRead(destination, backingIova)" in TICS
assert "guest.mappings.clear()" in TICS
# Critical regression: never reintroduce the #472 strategy of making sparseMap a normal translated guest mapping.
lookup = ADDR[ADDR.index("LookupBlockLocked("):ADDR.index("public:", ADDR.index("LookupBlockLocked("))]
assert "span<u8>{sparseMap" not in lookup

print("PASS: sampled sparse textures use GMMU reconstruction, never fake process-memory mappings")
