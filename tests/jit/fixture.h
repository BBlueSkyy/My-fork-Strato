#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <elf.h>
template<class T> void Put(std::vector<std::uint8_t> &v, size_t at, T value) { std::memcpy(v.data()+at,&value,sizeof(value)); }
std::vector<std::uint8_t> Fixture() {
    std::vector<std::uint8_t> v(0x3000);
    Put(v,4,std::uint32_t{0x100}); Put(v,0x10,std::uint32_t{0x304f524e});
    Put(v,0x18,std::uint32_t{0x3000});
    Put(v,0x20,std::uint32_t{0}); Put(v,0x24,std::uint32_t{0x1000});
    Put(v,0x28,std::uint32_t{0x1000}); Put(v,0x2c,std::uint32_t{0x1000});
    Put(v,0x30,std::uint32_t{0x2000}); Put(v,0x34,std::uint32_t{0x1000});
    Put(v,0x38,std::uint32_t{0x1000});
    Put(v,0x100,std::uint32_t{0x30444f4d}); Put(v,0x104,std::int32_t{0x1000});
    const std::pair<int,std::uint64_t> tags[] = {{DT_SYMTAB,0x1400},{DT_STRTAB,0x1600},{DT_STRSZ,64},{DT_SYMENT,24},{DT_HASH,0x1700},{DT_RELA,0x1800},{DT_RELASZ,24},{DT_RELAENT,24},{36,0x1900},{35,16},{37,8},{DT_NULL,0}};
    size_t at=0x1100; for(auto [tag,val]:tags) { Put(v,at,Elf64_Dyn{tag,{val}}); at+=16; }
    Put(v,0x1700,std::uint32_t{1}); Put(v,0x1704,std::uint32_t{2});
    Put(v,0x1400+24,Elf64_Sym{1,ELF64_ST_INFO(STB_GLOBAL,STT_FUNC),0,1,0x200,8});
    const char name[]="\0nnjitpluginGetVersion\0"; std::memcpy(v.data()+0x1600,name,sizeof(name));
    Put(v,0x1800,Elf64_Rela{0x2000,ELF64_R_INFO(0,R_AARCH64_RELATIVE),0x1234});
    Put(v,0x2000,std::uint64_t{0x9999}); // RELA replaces, not adds to, the existing contents.
    Put(v,0x1900,std::uint64_t{0x2010}); Put(v,0x1908,std::uint64_t{1|2|8});
    Put(v,0x2010,std::uint64_t{0x400}); Put(v,0x2018,std::uint64_t{0x500}); Put(v,0x2028,std::uint64_t{0x600});
    return v;
}
