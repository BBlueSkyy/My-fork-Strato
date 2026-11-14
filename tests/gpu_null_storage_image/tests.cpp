// SPDX-License-Identifier: MPL-2.0
// Exercises the production storage image null-texture policy header on the host.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <gpu/interconnect/common/null_texture_policy.h>
#include <gpu/interconnect/common/tic_validation_policy.h>

using namespace skyline::gpu::interconnect;
using R = NullTextureReason;
using A = StorageImageNullAction;

static int failures{};

#define EXPECT(expr)                                                                \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
            failures++;                                                             \
        }                                                                           \
    } while (false)

// The table is evaluated at compile time too, the policy must stay constexpr
static_assert(ResolveStorageImageNullAction(R::None, true) == A::UseGuestTexture);
static_assert(ResolveStorageImageNullAction(R::Unmapped, true) == A::BindNullDescriptor);
static_assert(IsNullBindlessHandle(0xFFFFFFFFU));
static_assert(!IsNullBindlessHandle(0U));
static_assert(NullBindlessTextureIndex == 0xFFFFFU);
static_assert(NullBindlessSamplerIndex == 0xFFFU);
static_assert(IsValidTicTextureType(0) && IsValidTicTextureType(8));
static_assert(!IsValidTicTextureType(9) && !IsValidTicTextureType(15));
static_assert(ShouldNullInvalidTic(15, false));
static_assert(!ShouldNullInvalidTic(15, true));

int main() {
    // Every reserved Maxwell TIC texture target (9..15) is invalid.
    // Missing base backing enables the sampled compatibility policy only.
    for (std::uint32_t type = 0; type < 16; ++type) {
        EXPECT(IsValidTicTextureType(type) == (type <= 8));
        EXPECT(ShouldNullInvalidTic(type, false) == (type > 8));
        EXPECT(!ShouldNullInvalidTic(type, true));
    }

    for (bool nullDescriptor : {false, true}) {
        // A guest-backed view is never replaced
        EXPECT(ResolveStorageImageNullAction(R::None, nullDescriptor) == A::UseGuestTexture);

        // Surfaces the guest expects to exist must never be silently discarded
        EXPECT(ResolveStorageImageNullAction(R::UnsupportedFormat, nullDescriptor) == A::Reject);
        EXPECT(ResolveStorageImageNullAction(R::PoolIndexOutOfRange, nullDescriptor) == A::Reject);
        EXPECT(ResolveStorageImageNullAction(R::InvalidDescriptor, nullDescriptor) == A::Reject);
    }

    // Unmapped GPU memory: only expressible faithfully with a null descriptor
    EXPECT(ResolveStorageImageNullAction(R::Unmapped, true) == A::BindNullDescriptor);
    EXPECT(ResolveStorageImageNullAction(R::Unmapped, false) == A::Reject);

    // The all-ones Maxwell bindless sentinel is distinct from arbitrary bad indices.
    EXPECT(IsNullBindlessHandle(NullBindlessHandleRaw));
    EXPECT(!IsNullBindlessHandle(NullBindlessHandleRaw - 1));

    // Out-of-range enum values must fail safe rather than bind anything
    EXPECT(ResolveStorageImageNullAction(static_cast<R>(0xFF), true) == A::Reject);

    // The diagnostic name tables in pipeline.inc are indexed by these values
    EXPECT(static_cast<int>(R::None) == 0 && static_cast<int>(R::PoolIndexOutOfRange) == 1 &&
           static_cast<int>(R::UnsupportedFormat) == 2 && static_cast<int>(R::Unmapped) == 3);
    EXPECT(static_cast<int>(A::UseGuestTexture) == 0 && static_cast<int>(A::BindNullDescriptor) == 1 && static_cast<int>(A::Reject) == 2);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("null storage image policy: all checks passed");
    return EXIT_SUCCESS;
}
