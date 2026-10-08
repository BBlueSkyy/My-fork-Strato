// SPDX-License-Identifier: MPL-2.0
#include <services/jit/plugin_image.h>
#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
using namespace skyline::service::jit;
#include "fixture.h"
std::uint64_t Read(const PluginImage &p,size_t off) { std::uint64_t out; std::memcpy(&out,p.Bytes().data()+off,8); return out; }
int main() {
    auto v=Fixture(); PluginImage p; p.Load(v,0x10000,[](std::string_view){return std::uint64_t{0};});
    assert(p.Bytes().size()==0x4000); assert(Read(p,0x3000)==0);
    assert(p.Symbol("nnjitpluginGetVersion")==0x10200);
    assert(Read(p,0x2000)==0x11234); assert(Read(p,0x2010)==0x10400);
    assert(Read(p,0x2018)==0x10500); assert(Read(p,0x2028)==0x10600);
    auto fails=[](std::vector<std::uint8_t> bytes) { try { PluginImage bad; bad.Load(bytes,0x10000,[](std::string_view){return std::uint64_t{0};}); } catch(const std::exception &) { return true; } return false; };
    auto bad=v; Put(bad,0x1800,Elf64_Rela{0x3ffc,ELF64_R_INFO(0,R_AARCH64_RELATIVE),0}); assert(fails(bad));
    bad=v; Put(bad,0x104,std::int32_t{-0x200}); assert(fails(bad));
    bad=v; Put(bad,0x1800,Elf64_Rela{0x2000,ELF64_R_INFO(0,9999),0}); assert(fails(bad));
    bad=v; Put(bad,0x1800,Elf64_Rela{0x2000,ELF64_R_INFO(1,R_AARCH64_GLOB_DAT),4});
    PluginImage globals; globals.Load(bad,0x10000,[](std::string_view){return std::uint64_t{0};}); assert(Read(globals,0x2000)==0x10204);
    Put(bad,0x1400+24,Elf64_Sym{1,ELF64_ST_INFO(STB_GLOBAL,STT_FUNC),0,SHN_UNDEF,0,0}); assert(fails(bad));
    PluginImage imported; imported.Load(bad,0x10000,[](std::string_view name){assert(name=="nnjitpluginGetVersion");return std::uint64_t{0x80000};}); assert(Read(imported,0x2000)==0x80004);
    auto terminal=v; Put(terminal,0x1900,std::uint64_t{0x3ff0}); Put(terminal,0x1908,std::uint64_t{3});
    PluginImage terminalImage; terminalImage.Load(terminal,0x10000,[](std::string_view){return std::uint64_t{};});
    assert(Read(terminalImage,0x3ff8)==0x10000);
    auto nullSymbol=v; Put(nullSymbol,0x1800,Elf64_Rela{0x2000,ELF64_R_INFO(0,R_AARCH64_ABS64),0x1234});
    PluginImage nullImage; nullImage.Load(nullSymbol,0x10000,[](std::string_view){assert(false);return std::uint64_t{};});
    assert(Read(nullImage,0x2000)==0x1234);
    std::cout << "plugin image: relocation, BSS, symbols, malformed input and imports passed\n";
}
