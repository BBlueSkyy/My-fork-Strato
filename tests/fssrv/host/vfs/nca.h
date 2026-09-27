// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vfs/backing.h>

namespace skyline::crypto {
    class KeyStore {
      public:
        explicit KeyStore(const std::string &) {}
    };
}

namespace skyline::vfs {
    class NCA {
      public:
        struct Header {
            u64 titleId{};
        } header;

        std::shared_ptr<Backing> romFs;

        NCA(std::shared_ptr<Backing>, std::shared_ptr<crypto::KeyStore>) {}
    };
}
