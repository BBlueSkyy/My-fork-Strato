#!/usr/bin/env python3
"""Regression gate for sparse GMMU TranslateRange semantics."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "app/src/main/cpp/skyline/common/address_space.h").read_text()
IMPL = (ROOT / "app/src/main/cpp/skyline/common/address_space.inc").read_text()

def between(text, start, end):
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]

fast = between(HEADER, "LookupBlockLocked(", "\n      public:")
slow = between(IMPL, "MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl(", "\n    MM_MEMBER()::FlatMemoryManager(")

assert "blockEntry.extraInfo.sparseMapped" in fast
assert "span<u8>{sparseMap, blockEntry.extent}" in fast
assert "blockEntry.phys == nullptr" in fast
assert "span<u8>{static_cast<u8*>(nullptr), blockEntry.extent}" in fast
assert fast.index("blockEntry.extraInfo.sparseMapped") < fast.index("blockEntry.phys == nullptr")

assert "predecessor->extraInfo.sparseMapped" in slow
assert "blockPhys = sparseMap" in slow
assert "span<u8>{static_cast<u8*>(nullptr), blockSize}" in slow
assert "SparsePlaceholderAddress()" in slow  # comment guards the CPU-pointer regression explicitly

# The old regression made fast sparse mappings invalid and the slow path could
# retain the 0xCAFEBABE placeholder. Both paths must resolve sparse VA through
# the zero-filled sparseMap while preserving invalid spans for true unmapped VA.
print("PASS: sparse TranslateRange remains distinct from unmapped VA")
