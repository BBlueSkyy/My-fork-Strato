// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#include <cstring>
#include <zstd.h>
#include "zbic.h"

namespace skyline::loader::zbic {
    namespace {
        constexpr u32 ZbicMagic{0x4349425A};
    }

    void Decompress(const std::vector<u8> &compressed, std::vector<u8> &output) {
        static_assert(ZSTD_VERSION_NUMBER == 10507, "ZBIC decoder is tied to Zstandard 1.5.7");

        if (compressed.size() < sizeof(u32))
            throw exception("Malformed ZBIC frame: frame is too short");

        u32 magic{};
        std::memcpy(&magic, compressed.data(), sizeof(magic));
        if (magic != ZbicMagic)
            throw exception("Malformed ZBIC frame: invalid magic 0x{:08X}", magic);

        const size_t result{ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size())};
        if (ZSTD_isError(result))
            throw exception("Failed to decompress ZBIC NSO segment: {}", ZSTD_getErrorName(result));

        if (result != output.size())
            throw exception("ZBIC NSO segment size mismatch (0x{:X}/0x{:X})", result, output.size());
    }
}
