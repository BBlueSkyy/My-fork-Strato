// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <vfs/backing.h>
#include <vfs/region_backing.h>
namespace skyline::crypto {
    class KeyStore { public: explicit KeyStore(const std::string &) {} };
}
namespace skyline::vfs {
    enum class NCAContentType { Program, Data };
    // Production NCA parsing/decryption is covered by tests/vfs. This facade lets
    // content-reader mount/error tests run without console keys or encrypted data.
    class NCA {
      public:
        struct { u64 titleId{}; } header;
        NCAContentType contentType{NCAContentType::Program};
        std::shared_ptr<Backing> romFs;
        NCA(std::shared_ptr<Backing> backing, std::shared_ptr<crypto::KeyStore>) {
            // Tiny host envelope, deliberately not an NCA parser. Exercise the
            // production source-selection policy with distinct title/type pairs.
            header.titleId = backing->Read<u64>(0);
            contentType = static_cast<NCAContentType>(backing->Read<u32>(8));
            romFs = std::make_shared<RegionBacking>(backing, 16, backing->size - 16);
        }
    };
}
