// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <gpu/diagnostic/re5_cubemap_probe.h>

using skyline::gpu::diagnostic::DiagnosticSubresourceFits;
using skyline::gpu::diagnostic::DiagnosticCubeCandidate;

int main() {
    static_assert(DiagnosticSubresourceFits(8, 0, 8));
    static_assert(!DiagnosticSubresourceFits(1, 0, 8));
    static_assert(DiagnosticSubresourceFits(1, 0, 1));
    static_assert(!DiagnosticSubresourceFits(8, 8, 1));
    static_assert(!DiagnosticSubresourceFits(8, 0, 0));
    static_assert(DiagnosticSubresourceFits(8, 3, UINT32_MAX));
    static_assert(!DiagnosticSubresourceFits(1, 1, UINT32_MAX));
    constexpr auto candidate = [](std::size_t bytes, std::uint32_t layers, bool transfer) {
        return DiagnosticCubeCandidate(128, 128, 1, layers, 8, bytes, true, transfer, true);
    };
    static_assert(candidate(524280, 6, true));
    static_assert(!candidate(524280, 5, true));
    static_assert(!candidate(0, 6, true));
    static_assert(!candidate(524280, 6, false));
    static_assert(!candidate(3ULL * 1024 * 1024, 6, true));
    assert(DiagnosticCubeCandidate(128, 128, 1, 6, 1, 393216, true, true, true));
    assert(!DiagnosticCubeCandidate(256, 128, 1, 6, 1, 393216, true, true, true));
    return 0;
}
