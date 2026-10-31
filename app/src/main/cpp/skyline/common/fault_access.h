// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace skyline::fault_diagnostics {
    // Observation only: 0=data read, 1=data write, 2=unknown. Linux arm64 signal
    // records: ESR_MAGIC 0x45535201; ESR EC=0x24/0x25, WnR bit6.
    inline unsigned Access(std::span<const std::byte> records) {
        size_t offset{};
        while(records.size()-offset>=8) {
            uint32_t magic{},size{};
            std::memcpy(&magic,records.data()+offset,4);
            std::memcpy(&size,records.data()+offset+4,4);
            if(!magic && !size)return 2;
            if(size<16 || size%16 || size>records.size()-offset)return 2;
            if(magic==0x45535201) {
                if(size!=16)return 2;
                uint64_t esr{};std::memcpy(&esr,records.data()+offset+8,8);
                const auto ec{(esr>>26)&63};
                // Cache-maintenance aborts are not classified as CPU data stores.
                if(esr&(1ULL<<8))return 2;
                return ec==0x24 || ec==0x25 ? unsigned((esr>>6)&1) : 2;
            }
            offset+=size;
        }
        return 2;
    }
}
