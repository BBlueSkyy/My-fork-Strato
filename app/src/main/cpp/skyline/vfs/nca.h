// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <common.h>

#include <crypto/aes_cipher.h>
#include <loader/loader.h>

#include "nca.h"
#include "bktr_backing.h"
#include "ctr_encrypted_backing.h"
#include "region_backing.h"
#include "partition_filesystem.h"

namespace skyline::vfs {
    std::shared_ptr<Backing> NCA::CreateBacking(const NcaSectionHeader &sectionHeader, std::shared_ptr<Backing> rawBacking, size_t offset) {
        if (!encrypted)
            return rawBacking;

        switch (sectionHeader.encryptionType) {
            case NcaSectionEncryptionType::None:
                return rawBacking;

            case NcaSectionEncryptionType::CTR: {
                auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.encryptionType)};

                // Construct AES-CTR base value from secureValue/generation (little-endian layout)
                std::array<u8, 0x10> ctr{};
                u32 secureValueLE{util::SwapEndianness(sectionHeader.secureValue)};
                u32 generationLE{util::SwapEndianness(sectionHeader.generation)};
                std::memcpy(ctr.data(), &secureValueLE, sizeof(u32));
                std::memcpy(ctr.data() + 4, &generationLE, sizeof(u32));

                return std::make_shared<CtrEncryptedBacking>(ctr, key, std::move(rawBacking), offset);
            }

            case NcaSectionEncryptionType::BKTR: {
                auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.encryptionType)};

                return std::make_shared<BktrBacking>(
                    key,
                    sectionHeader.secureValue,
                    sectionHeader.generation,
                    std::move(rawBacking),
                    offset,
                    sectionHeader.patchInfo.indirectOffset,
                    sectionHeader.patchInfo.indirectSize,
                    sectionHeader.patchInfo.aesCtrExOffset,
                    sectionHeader.patchInfo.aesCtrExSize
                );
            }

            default:
                return nullptr;
        }
    }
}

