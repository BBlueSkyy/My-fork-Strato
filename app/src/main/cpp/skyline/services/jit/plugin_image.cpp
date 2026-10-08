// SPDX-License-Identifier: MPL-2.0
#include "plugin_image.h"
#include <cstring>
#include <elf.h>
#include <limits>
#include <stdexcept>

namespace skyline::service::jit {
    namespace {
        bool Contains(std::size_t capacity, std::uint64_t offset, std::uint64_t size) {
            return offset <= capacity && size <= capacity - offset;
        }
        template<class T> T Read(std::span<const std::uint8_t> bytes, std::uint64_t offset) {
            if (!Contains(bytes.size(), offset, sizeof(T)))
                throw std::runtime_error("JIT plugin table outside image");
            T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
        }
        void Write(std::vector<std::uint8_t> &image, std::uint64_t offset, std::uint64_t value) {
            if (!Contains(image.size(), offset, sizeof(value)) || offset % 8)
                throw std::runtime_error("JIT plugin relocation outside image or unaligned");
            std::memcpy(image.data() + offset, &value, sizeof(value));
        }
        std::string_view Name(std::span<const std::uint8_t> bytes, std::uint64_t offset, std::uint64_t capacity) {
            if (!Contains(bytes.size(), offset, capacity))
                throw std::runtime_error("JIT plugin string table outside image");
            auto *start = reinterpret_cast<const char *>(bytes.data() + offset);
            auto *end = static_cast<const char *>(std::memchr(start, 0, capacity));
            if (!end) throw std::runtime_error("JIT plugin unterminated symbol name");
            return {start, static_cast<std::size_t>(end - start)};
        }
    }

    std::uint64_t PluginImage::Symbol(std::string_view name) const {
        const auto it = symbols.find(name);
        return it == symbols.end() ? 0 : it->second;
    }

    void PluginImage::Load(std::span<const std::uint8_t> nro, std::uint64_t loadBase, const Resolver &resolver) {
        // Parse into a temporary image so a rejected load leaves this object untouched.
        PluginImage loaded;
        if (nro.size() < 0x80 || Read<std::uint32_t>(nro, 0x10) != 0x304f524e)
            throw std::runtime_error("Invalid JIT NRO0 header");
        const auto fileSize = Read<std::uint32_t>(nro, 0x18);
        const auto bss = Read<std::uint32_t>(nro, 0x38);
        if (fileSize < 0x80 || fileSize > nro.size())
            throw std::runtime_error("Invalid JIT NRO size");
        std::uint64_t next{};
        for (std::size_t at = 0x20; at < 0x38; at += 8) {
            const auto offset = Read<std::uint32_t>(nro, at);
            const auto size = Read<std::uint32_t>(nro, at + 4);
            if (offset != next || offset % 0x1000 || size % 0x1000 || !Contains(fileSize, offset, size))
                throw std::runtime_error("Invalid JIT NRO segment layout");
            next = static_cast<std::uint64_t>(offset) + size;
        }
        if (next != fileSize || !Read<std::uint32_t>(nro,0x24) ||
            fileSize + static_cast<std::uint64_t>(bss) > std::numeric_limits<std::size_t>::max() ||
            loadBase > std::numeric_limits<std::uint64_t>::max() - fileSize - bss)
            throw std::runtime_error("Invalid JIT NRO memory size");
        loaded.base = loadBase;
        loaded.textSize = Read<std::uint32_t>(nro, 0x24);
        loaded.image.resize(static_cast<std::size_t>(fileSize) + bss);
        std::memcpy(loaded.image.data(), nro.data(), fileSize);
        nro = nro.first(fileSize);

        const auto mod = Read<std::uint32_t>(nro,4);
        if (!mod || Read<std::uint32_t>(nro,mod) != 0x30444f4d)
            throw std::runtime_error("Invalid JIT MOD0 header");
        const auto dynamicOffset = static_cast<std::int64_t>(mod) + Read<std::int32_t>(nro,mod+4);
        if (dynamicOffset < 0) throw std::runtime_error("Invalid JIT MOD0 dynamic offset");
        std::map<std::int64_t,std::uint64_t> tags;
        bool terminated{};
        for (std::uint64_t at = dynamicOffset; Contains(nro.size(),at,sizeof(Elf64_Dyn)); at += sizeof(Elf64_Dyn)) {
            const auto dyn = Read<Elf64_Dyn>(nro,at);
            if (dyn.d_tag == DT_NULL) { terminated=true; break; }
            if (dyn.d_tag == DT_NEEDED) throw std::runtime_error("JIT plugin dependencies are unavailable");
            tags[dyn.d_tag] = dyn.d_un.d_val;
        }
        if (!terminated) throw std::runtime_error("Unterminated JIT dynamic table");

        // MOD0 is authoritative; retain the NRO header table fallback from #324.
        std::uint64_t symtab=tags[DT_SYMTAB], strtab=tags[DT_STRTAB], strsz=tags[DT_STRSZ], count{};
        if (symtab && strtab && strsz && tags[DT_SYMENT] == sizeof(Elf64_Sym)) {
            if (tags[DT_HASH]) {
                const auto hash=tags[DT_HASH];
                const auto buckets=Read<std::uint32_t>(nro,hash);
                count=Read<std::uint32_t>(nro,hash+4);
                if (!Contains(nro.size(),hash,8 + (static_cast<std::uint64_t>(buckets)+count)*4))
                    throw std::runtime_error("Invalid JIT SysV hash table");
            } else if (tags[DT_GNU_HASH]) {
                const auto hash=tags[DT_GNU_HASH];
                const auto buckets=Read<std::uint32_t>(nro,hash), first=Read<std::uint32_t>(nro,hash+4);
                const auto bloom=Read<std::uint32_t>(nro,hash+8);
                const auto bucketOffset=hash+16+static_cast<std::uint64_t>(bloom)*8;
                const auto chainOffset=bucketOffset+static_cast<std::uint64_t>(buckets)*4;
                if (!buckets || !bloom || !Contains(nro.size(),bucketOffset,static_cast<std::uint64_t>(buckets)*4))
                    throw std::runtime_error("Invalid JIT GNU hash table");
                count=first;
                for (std::uint64_t i=0;i<buckets;i++) {
                    std::uint64_t index=Read<std::uint32_t>(nro,bucketOffset+i*4);
                    if (!index) continue;
                    if (index<first) throw std::runtime_error("Invalid JIT GNU hash bucket");
                    while (!(Read<std::uint32_t>(nro,chainOffset+(index-first)*4)&1)) ++index;
                    count=std::max(count,index+1);
                }
            } else if (strtab > symtab && (strtab-symtab)%sizeof(Elf64_Sym)==0) {
                count=(strtab-symtab)/sizeof(Elf64_Sym);
            }
        }
        if (!count) {
            symtab=Read<std::uint32_t>(nro,0x78);
            const auto size=Read<std::uint32_t>(nro,0x7c);
            strtab=Read<std::uint32_t>(nro,0x70); strsz=Read<std::uint32_t>(nro,0x74);
            if (size%sizeof(Elf64_Sym)) throw std::runtime_error("Invalid JIT NRO symbol size");
            count=size/sizeof(Elf64_Sym);
        }
        if (!count || count>nro.size()/sizeof(Elf64_Sym) || !Contains(nro.size(),symtab,count*sizeof(Elf64_Sym)) ||
            !strsz || !Contains(nro.size(),strtab,strsz))
            throw std::runtime_error("Invalid JIT dynamic symbol table");
        const auto symbolName=[&](const Elf64_Sym &sym) {
            if (sym.st_name>=strsz) throw std::runtime_error("Invalid JIT symbol name offset");
            return Name(nro,strtab+sym.st_name,strsz-sym.st_name);
        };
        const auto address=[&](const Elf64_Sym &sym) -> std::uint64_t {
            if (sym.st_shndx==SHN_ABS) return sym.st_value;
            if (sym.st_shndx==SHN_UNDEF) {
                auto value=resolver(symbolName(sym));
                if (!value && ELF64_ST_BIND(sym.st_info)!=STB_WEAK)
                    throw std::runtime_error("Unresolved JIT plugin import: " + std::string(symbolName(sym)));
                return value;
            }
            if (sym.st_shndx>=SHN_LORESERVE || !Contains(loaded.image.size(),sym.st_value,sym.st_size))
                throw std::runtime_error("Invalid JIT symbol address");
            return loadBase+sym.st_value;
        };
        for (std::uint64_t i=1;i<count;i++) {
            const auto sym=Read<Elf64_Sym>(nro,symtab+i*sizeof(Elf64_Sym));
            const auto name=symbolName(sym);
            if (sym.st_shndx!=SHN_UNDEF && !name.empty()) loaded.symbols.emplace(name,address(sym));
        }
        const auto relocate=[&](std::uint64_t table,std::uint64_t size) {
            if (!size) return;
            if (!table || size%sizeof(Elf64_Rela) || !Contains(nro.size(),table,size))
                throw std::runtime_error("Invalid JIT RELA table");
            for (std::uint64_t at=table;at<table+size;at+=sizeof(Elf64_Rela)) {
                const auto rel=Read<Elf64_Rela>(nro,at);
                const auto type=ELF64_R_TYPE(rel.r_info), index=ELF64_R_SYM(rel.r_info);
                if (type==R_AARCH64_NONE) continue;
                std::uint64_t value{};
                if (type==R_AARCH64_RELATIVE) {
                    if (index) throw std::runtime_error("Invalid JIT RELATIVE symbol");
                    value=loadBase+rel.r_addend;
                } else if (type==R_AARCH64_ABS64 || type==R_AARCH64_GLOB_DAT || type==R_AARCH64_JUMP_SLOT) {
                    if (index>=count) throw std::runtime_error("Invalid JIT relocation symbol index");
                    value=(index ? address(Read<Elf64_Sym>(nro,symtab+index*sizeof(Elf64_Sym))) : 0)+rel.r_addend;
                } else throw std::runtime_error("Unsupported JIT AArch64 relocation");
                Write(loaded.image,rel.r_offset,value);
            }
        };
        if (tags[DT_RELASZ] && tags[DT_RELAENT]!=sizeof(Elf64_Rela))
            throw std::runtime_error("Invalid JIT RELA entry size");
        relocate(tags[DT_RELA],tags[DT_RELASZ]);
        if (tags[DT_PLTRELSZ]) {
            if (tags[DT_PLTREL]!=DT_RELA) throw std::runtime_error("Unsupported JIT PLT relocation encoding");
            relocate(tags[DT_JMPREL],tags[DT_PLTRELSZ]);
        }
        // ELF DT_RELR/DT_RELRSZ/DT_RELRENT (including on older Android elf.h).
        const auto relr=tags[36], relrsz=tags[35];
        if (relrsz && (tags[37]!=8 || relrsz%8 || !Contains(nro.size(),relr,relrsz)))
            throw std::runtime_error("Invalid JIT RELR table");
        std::uint64_t where{}; bool haveWhere{};
        const auto rebase=[&](std::uint64_t offset) { Write(loaded.image,offset,Read<std::uint64_t>(loaded.image,offset)+loadBase); };
        for (std::uint64_t at=relr;at<relr+relrsz;at+=8) {
            const auto entry=Read<std::uint64_t>(nro,at);
            if (!(entry&1)) { where=entry; rebase(where); where+=8; haveWhere=true; }
            else {
                if (!haveWhere || where>std::numeric_limits<std::uint64_t>::max()-63*8)
                    throw std::runtime_error("Invalid JIT RELR bitmap");
                for (unsigned bit=1;bit<64;bit++) if (entry&(1ULL<<bit)) rebase(where+(bit-1)*8);
                where+=63*8;
            }
        }
        if (tags[DT_RELSZ]) throw std::runtime_error("Unsupported JIT REL encoding");
        const auto addFunctions=[&](auto &out,std::int64_t single,std::int64_t array,std::int64_t size,std::string_view fallback) {
            const auto fn=tags[single] ? loadBase+tags[single] : loaded.Symbol(fallback);
            if (fn) out.push_back(fn);
            if (tags[size]%8 || !Contains(loaded.image.size(),tags[array],tags[size]))
                throw std::runtime_error("Invalid JIT constructor table");
            for (std::uint64_t i=0;i<tags[size];i+=8) {
                const auto entry=Read<std::uint64_t>(loaded.image,tags[array]+i);
                if (entry && entry!=std::numeric_limits<std::uint64_t>::max()) out.push_back(entry);
            }
            for (auto value:out) if (value<loadBase || value-loadBase>=loaded.textSize || value%4)
                throw std::runtime_error("JIT constructor outside text");
        };
        addFunctions(loaded.initializers,DT_INIT,DT_INIT_ARRAY,DT_INIT_ARRAYSZ,"_init");
        addFunctions(loaded.finalizers,DT_FINI,DT_FINI_ARRAY,DT_FINI_ARRAYSZ,"_fini");
        *this=std::move(loaded);
    }
}
