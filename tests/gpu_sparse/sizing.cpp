#include <gpu/interconnect/common/sparse_texture.h>
#include <cassert>
#include <iostream>
#include <limits>
using skyline::gpu::interconnect::SparseRgba8MipBytes;
using skyline::gpu::interconnect::SparseMipViewFits;
int main() {
    assert(SparseMipViewFits(12, 1, 11));
    assert(SparseMipViewFits(12, 3, 9));
    assert(SparseMipViewFits(12, 5, 7));
    assert(SparseMipViewFits(12, 11, 1));
    assert(!SparseMipViewFits(12, 12, 1));
    assert(!SparseMipViewFits(12, 0, 0));
    assert(!SparseMipViewFits(12, 3, 10));
    assert(!SparseMipViewFits(12, UINT32_MAX, 1));
    assert(!SparseMipViewFits(12, 1, UINT32_MAX));
    assert(SparseRgba8MipBytes(2048, 2048, 12) == 22369620);
    assert(SparseRgba8MipBytes(512, 2048, 12) == 5592412);
    assert(*SparseRgba8MipBytes(2048, 2048, 12) + *SparseRgba8MipBytes(512, 2048, 12) < 32 * 1024 * 1024);
    assert(SparseRgba8MipBytes(1, 1, 1) == 4);
    assert(SparseRgba8MipBytes(3, 5, 3) == 72); // NPOT: 3x5, 1x2, 1x1
    assert(SparseRgba8MipBytes(1, 2048, 12) == 16380);
    assert(!SparseRgba8MipBytes(0, 1, 1));
    assert(!SparseRgba8MipBytes(1, 0, 1));
    assert(!SparseRgba8MipBytes(2048, 2048, 0));
    assert(!SparseRgba8MipBytes(2048, 2048, 13));
    assert(!SparseRgba8MipBytes(1, 1, 32));
    assert(!SparseRgba8MipBytes(UINT32_MAX, UINT32_MAX, 1));
    for (uint32_t w = 1; w <= 2048; w *= 2)
        for (uint32_t h = 1; h <= 2048; h *= 2) {
            size_t expected{};
            auto mw = w, mh = h;
            for (uint32_t level = 1;; ++level) {
                expected += size_t(mw) * mh * 4;
                assert(SparseRgba8MipBytes(w, h, level) == expected);
                if (mw == 1 && mh == 1) break;
                mw = std::max(1u, mw / 2); mh = std::max(1u, mh / 2);
            }
        }
    std::cout << "PASS: RGBA8 mip-chain sizes, NPOT, tails, invalid mip counts, overflow\n";
}
