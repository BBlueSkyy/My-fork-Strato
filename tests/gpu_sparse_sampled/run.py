#!/usr/bin/env python3
"""Guard the sampled sparse texture contract against the unsafe PR #472 design."""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADDR = (ROOT / "app/src/main/cpp/skyline/common/address_space.h").read_text()
TEXH = (ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.h").read_text()
TEX = (ROOT / "app/src/main/cpp/skyline/gpu/texture/texture.cpp").read_text()
MANAGER = (ROOT / "app/src/main/cpp/skyline/gpu/texture_manager.cpp").read_text()
TICS = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/common/textures.cpp").read_text()

assert "bool GetResidentMappings(VaType virt, VaType size" in ADDR
assert "u64 GetMappingGeneration() const" in ADDR
assert "std::atomic<u64> mappingGeneration" in ADDR
assert "bool VisitResident(VaType virt, VaType size, u64 &generation" in ADDR
# The GMMU must never copy guest memory itself (TryRead held the block lock across a faulting memcpy).
assert "TryRead" not in ADDR
assert "predecessor->extraInfo.sparseMapped" in ADDR
assert "if (predecessor->phys == nullptr)" in ADDR

assert "bool sparseReadOnly" in TEXH
assert "sparseSegments" in TEXH
assert not re.search(r"\bsparseRead\b", TEXH)
assert "std::function<bool(Mappings &, u64 &)> sparseMappings" in TEXH
assert "std::function<u64()> sparseGeneration" in TEXH
assert "sparseMappingGeneration" in TEXH
assert "if (guest->sparseReadOnly)" in TEX
assert "sparse holes never become process-memory mappings" in TEX
assert "ReadSparseBacking(sparseBacking)" in TEX
assert not re.search(r"\bsparseRead\b", TEX)
assert "bool Texture::RefreshSparseMappings()" in TEX
assert "guest->sparseGeneration()" in TEX
assert "CreateTrap(guest->mappings" in TEX
assert "TrapRegions(*trapHandle, true)" in TEX
assert "Re-read sparse residency/content once per submission" not in TEX
assert "Conservative correctness: remaps/writes are observed at submission" not in TEX

sparse_branch = MANAGER[MANAGER.index("if (guestTexture.sparseReadOnly)"):MANAGER.index("/*", MANAGER.index("if (guestTexture.sparseReadOnly)"))]
assert "SetupGuestMappings" in sparse_branch
assert "aliasReadOnly = true" in sparse_branch
assert "GetView(" in sparse_branch

assert "supportedSparseSampled" in TICS
assert "GetResidentMappings(backingIova, sparseSize" in TICS
assert "guest.sparseMappings" in TICS
assert "guest.sparseGeneration" in TICS
assert "gmmu.VisitResident(backingIova, sparseSize" in TICS
assert "TryRead" not in TICS
# Critical regression: never reintroduce the #472 strategy of making sparseMap a normal translated guest mapping.
lookup = ADDR[ADDR.index("LookupBlockLocked("):ADDR.index("public:", ADDR.index("LookupBlockLocked("))]
assert "span<u8>{sparseMap" not in lookup

print("PASS: sampled sparse textures use GMMU reconstruction, never fake process-memory mappings")

# Sparse creation must not emit an eager standalone layout-transition submission.
sparse_branch = MANAGER[MANAGER.index("if (guestTexture.sparseReadOnly)"):MANAGER.index("/*", MANAGER.index("if (guestTexture.sparseReadOnly)"))]
assert "TransitionLayout(" not in sparse_branch
assert "Leave the newly allocated image Undefined" in sparse_branch

# Streaming sparse TICs with non-zero view bases should materialize only an exact physical mip tail.
assert "Materialize only" in TICS or "Materialize only" in TICS.replace("materialize", "Materialize")
assert "exactTailLayout" in TICS
assert "tailOffset" in TICS
assert "guest = std::move(tail)" in TICS

# Startup-freeze regression (Oceanhorn 2): sparse uploads must read resident runs through a mirror, never the trapped guest pointer.
body = TEX[TEX.index("void Texture::ReadSparseBacking("):TEX.index("std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl()")]
assert "CopySparseSegments(" in body
assert "CreateMirror(" in body
assert "munmap(" in body
assert "guest->sparseGeneration() == generation" in body  # the GMMU lock is not held across the copy, so residency is re-validated
assert "std::memcpy" not in body and "memcpy(" not in body  # no direct guest dereference in the texture
impl = TEX[TEX.index("std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl()"):]
impl = impl[:impl.index("CopyBlockLinearToLinear(*guest")]
assert not re.search(r"\bsparseRead\b", impl) and "ReadSparseBacking(sparseBacking)" in impl
print("PASS: sparse backing is read through mirrors (no trap fault / self-wait in SubmitInternal)")

with tempfile.TemporaryDirectory(prefix="strato-sparse-self-wait-") as build:
    binary = Path(build) / "sparse_self_wait_test"
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-O1", "-I", str(ROOT / "app/src/main/cpp/skyline"),
                    str(ROOT / "tests/gpu_sparse_sampled/sparse_self_wait_test.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
