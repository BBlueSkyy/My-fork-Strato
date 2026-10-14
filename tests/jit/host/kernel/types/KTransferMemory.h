// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <kernel/types/KObject.h>
namespace skyline::kernel::type {
    class KTransferMemory : public KObject {
      public:
        span<u8> host,guest;
        KTransferMemory(const DeviceState &s,span<u8> source):KObject(s,KType::KTransferMemory),host(source),guest(source) {}
    };
}
