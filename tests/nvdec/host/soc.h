#pragma once
#include <common.h>
namespace skyline::soc {
struct HostMemory {
    std::vector<u8> bytes=std::vector<u8>(8*1024*1024,0xcd);
    std::vector<std::pair<u32,size_t>> writes;
    std::vector<span<u8>> TranslateRange(u32 address,u32 size) {
        if (u64(address)+size>bytes.size()) return {{static_cast<u8 *>(nullptr),size}};
        return {{bytes.data()+address,size}};
    }
    void Write(u32 address,span<u8> src) {
        if (u64(address)+src.size()>bytes.size()) throw std::out_of_range("SMMU write");
        std::memcpy(bytes.data()+address,src.data(),src.size()); writes.emplace_back(address,src.size());
    }
    template<class T> T Read(u32 address) {
        T value{}; if(u64(address)+sizeof(T)>bytes.size()) throw std::out_of_range("SMMU read");
        std::memcpy(&value,bytes.data()+address,sizeof(T)); return value;
    }
    void Read(span<u8> dst,u32 address) { std::memcpy(dst.data(),bytes.data()+address,dst.size()); }
};
struct HostSoc { HostMemory smmu; };
}
