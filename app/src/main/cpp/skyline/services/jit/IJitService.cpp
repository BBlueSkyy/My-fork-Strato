// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only JIT interface. This parses plugin metadata, but never runs guest code.

#include <algorithm>
#include <array>
#include <cstring>
#include <elf.h>
#include <optional>
#include <string_view>
#include <mbedtls/sha256.h>
#include <kernel/results.h>
#include <kernel/types/KTransferMemory.h>
#include <loader/nro.h>
#include "IJitService.h"

namespace skyline::service::jit {
    namespace {
        // nn::ro::detail::NrrHeader, see NRR0 on Switchbrew.
        constexpr size_t NrrHeaderSize{0x350};
        constexpr size_t NrrSizeOffset{0x338};
        constexpr size_t NrrKindOffset{0x33C};
        constexpr size_t NrrHashesOffset{0x340};
        constexpr size_t NrrHashesCount{0x344};
        constexpr size_t HashSize{0x20};

        bool ContainsBytes(span<u8> data, size_t offset, size_t size) {
            return offset <= data.size() && size <= data.size() - offset;
        }

        u32 ReadU32(span<u8> data, size_t offset) {
            u32 value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        struct DynamicSymbols {
            size_t symbolOffset;
            size_t symbolCount;
            size_t stringOffset;
            size_t stringSize;
            const char *source;
        };

        bool SymbolTableValid(span<u8> image, const DynamicSymbols &table) {
            return table.symbolCount > 0 && table.symbolCount <= image.size() / sizeof(Elf64_Sym) &&
                   ContainsBytes(image, table.symbolOffset, table.symbolCount * sizeof(Elf64_Sym)) &&
                   table.stringSize != 0 && ContainsBytes(image, table.stringOffset, table.stringSize);
        }

        std::optional<DynamicSymbols> ResolveDynamicSymbols(span<u8> image, const loader::NroHeader &header) {
            // NRO0 embeds offsets for .dynsym/.dynstr in the header, but some
            // binaries leave them unset. MOD0's ELF dynamic entries are the
            // authoritative fallback; interpreting missing header offsets as
            // "no exports" produced misleading diagnostics for JIT plugins.
            const DynamicSymbols headerTable{
                header.dynsym.offset,
                header.dynsym.size / sizeof(Elf64_Sym),
                header.dynstr.offset,
                header.dynstr.size,
                "NRO header"
            };

            LOGW("JIT_DIAG: NRO symbol header: mod=0x{:X}, dynsym=0x{:X}+0x{:X}, dynstr=0x{:X}+0x{:X}",
                 header.modOffset, header.dynsym.offset, header.dynsym.size,
                 header.dynstr.offset, header.dynstr.size);

            if (header.modOffset != 0 && ContainsBytes(image, header.modOffset, 8) &&
                ReadU32(image, header.modOffset) == util::MakeMagic<u32>("MOD0")) {
                // MOD0.DynamicOffset is signed and relative to the MOD0 base.
                const auto relative{static_cast<i32>(ReadU32(image, header.modOffset + 4))};
                const i64 dynamicAddress{static_cast<i64>(header.modOffset) + relative};
                if (dynamicAddress >= 0 &&
                    ContainsBytes(image, static_cast<size_t>(dynamicAddress), sizeof(Elf64_Dyn))) {
                    u64 symtab{}, strtab{}, strsz{}, syment{}, hash{};
                    bool terminated{};
                    for (size_t pos{static_cast<size_t>(dynamicAddress)};
                         ContainsBytes(image, pos, sizeof(Elf64_Dyn)); pos += sizeof(Elf64_Dyn)) {
                        Elf64_Dyn entry{};
                        std::memcpy(&entry, image.data() + pos, sizeof(entry));
                        if (entry.d_tag == DT_NULL) {
                            terminated = true;
                            break;
                        }
                        switch (entry.d_tag) {
                            case DT_SYMTAB: symtab = entry.d_un.d_ptr; break;
                            case DT_STRTAB: strtab = entry.d_un.d_ptr; break;
                            case DT_STRSZ: strsz = entry.d_un.d_val; break;
                            case DT_SYMENT: syment = entry.d_un.d_val; break;
                            case DT_HASH: hash = entry.d_un.d_ptr; break;
                            default: break;
                        }
                    }

                    if (terminated && syment == sizeof(Elf64_Sym) && symtab != 0 && strtab != 0 && strsz != 0) {
                        size_t count{};
                        // SysV DT_HASH gives the exact symbol count (nchain).
                        if (hash != 0 && hash <= image.size() && ContainsBytes(image, static_cast<size_t>(hash), 8))
                            count = ReadU32(image, static_cast<size_t>(hash) + 4);
                        // Some homebrew NROs have no SysV hash. When the
                        // string table immediately follows symbols, the
                        // difference is a safe upper bound for symbol count.
                        else if (strtab > symtab && (strtab - symtab) % sizeof(Elf64_Sym) == 0)
                            count = (strtab - symtab) / sizeof(Elf64_Sym);

                        if (symtab <= image.size() && strtab <= image.size() && strsz <= image.size()) {
                            const DynamicSymbols modTable{
                                static_cast<size_t>(symtab), count,
                                static_cast<size_t>(strtab), static_cast<size_t>(strsz), "MOD0 dynamic"
                            };
                            if (SymbolTableValid(image, modTable)) {
                                LOGW("JIT_DIAG: NRO MOD0 resolved symbols: count={}, dynsym=0x{:X}, dynstr=0x{:X}, hash=0x{:X}",
                                     count, symtab, strtab, hash);
                                return modTable;
                            }
                        }
                        LOGW("JIT_DIAG: NRO MOD0 symbol bounds/count invalid: sym=0x{:X}, str=0x{:X}, strsz=0x{:X}, n={}, hash=0x{:X}",
                             symtab, strtab, strsz, count, hash);
                    } else {
                        LOGW("JIT_DIAG: NRO MOD0 dynamic table invalid: terminated={}, syment=0x{:X}, symtab=0x{:X}, strtab=0x{:X}, strsz=0x{:X}",
                             terminated, syment, symtab, strtab, strsz);
                    }
                } else {
                    LOGW("JIT_DIAG: NRO MOD0 dynamic offset outside NRO: 0x{:X}", dynamicAddress);
                }
            } else {
                LOGW("JIT_DIAG: NRO has no valid MOD0 at offset 0x{:X}", header.modOffset);
            }

            // Prefer valid, bounded NRO header sections over heuristic scans.
            if (header.dynsym.size && header.dynsym.size % sizeof(Elf64_Sym) == 0 &&
                SymbolTableValid(image, headerTable)) {
                LOGW("JIT_DIAG: NRO symbols sourced from header (count={})", headerTable.symbolCount);
                return headerTable;
            }

            LOGW("JIT_DIAG: NRO exports cannot be resolved safely; no symbol claims will be made");
            return std::nullopt;
        }

        bool HasSymbol(span<u8> image, const DynamicSymbols &table, std::string_view name) {
            const auto strings{image.subspan(table.stringOffset, table.stringSize)};
            for (size_t i{}; i < table.symbolCount; i++) {
                Elf64_Sym symbol{};
                std::memcpy(&symbol, image.data() + table.symbolOffset + i * sizeof(Elf64_Sym), sizeof(symbol));
                if (symbol.st_name >= strings.size() || symbol.st_shndx == SHN_UNDEF)
                    continue;
                const auto *start{reinterpret_cast<const char *>(strings.data() + symbol.st_name)};
                const size_t capacity{strings.size() - symbol.st_name};
                const auto *end{static_cast<const char *>(std::memchr(start, 0, capacity))};
                if (end && std::string_view{start, static_cast<size_t>(end - start)} == name)
                    return true;
            }
            return false;
        }
    }

    IJitService::IJitService(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    IJitEnvironment::IJitEnvironment(const DeviceState &state, ServiceManager &manager,
                                     std::shared_ptr<kernel::type::KProcess> process,
                                     std::shared_ptr<kernel::type::KCodeMemory> executableMemory,
                                     std::shared_ptr<kernel::type::KCodeMemory> readableMemory)
        : BaseService(state, manager), process(std::move(process)),
          executableMemory(std::move(executableMemory)), readableMemory(std::move(readableMemory)) {}

    Result IJitService::CreateJitEnvironment(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.cmdArgSz < sizeof(u64) * 2)
            return kernel::result::InvalidArgument;

        const u64 executableSize{request.Pop<u64>()};
        const u64 readableSize{request.Pop<u64>()};
        LOGW("JIT_DIAG: CreateJitEnvironment exec_size=0x{:X}, ro_size=0x{:X}, copied_handles={}, moved_handles={}",
             executableSize, readableSize, request.copyHandles.size(), request.moveHandles.size());

        if (request.copyHandles.size() != 3) {
            LOGW("JIT_DIAG: CreateJitEnvironment expected exactly 3 copied handles");
            return kernel::result::InvalidHandle;
        }

        std::shared_ptr<kernel::type::KProcess> process;
        std::shared_ptr<kernel::type::KCodeMemory> executableMemory;
        std::shared_ptr<kernel::type::KCodeMemory> readableMemory;

        // The process and both CodeMemory handles are distinct kernel objects.
        // Validate them before exposing a diagnostic session to the guest.
        try {
            process = state.process->GetHandle<kernel::type::KProcess>(request.copyHandles.at(0));
            if (executableSize)
                executableMemory = state.process->GetHandle<kernel::type::KCodeMemory>(request.copyHandles.at(1));
            if (readableSize)
                readableMemory = state.process->GetHandle<kernel::type::KCodeMemory>(request.copyHandles.at(2));
        } catch (const std::exception &e) {
            LOGW("JIT_DIAG: CreateJitEnvironment invalid handle: {}", e.what());
            return kernel::result::InvalidHandle;
        }

        if (process != state.process ||
            (executableSize && (!executableMemory || executableSize > executableMemory->GetSource().size())) ||
            (readableSize && (!readableMemory || readableSize > readableMemory->GetSource().size()))) {
            LOGW("JIT_DIAG: CreateJitEnvironment process or CodeMemory size mismatch");
            return kernel::result::InvalidArgument;
        }

        LOGW("JIT_DIAG: validated JIT process and CodeMemory handles (exec=0x{:X}, ro=0x{:X}); mapping not implemented",
             executableSize, readableSize);
        manager.RegisterService(SRVREG(IJitEnvironment, std::move(process), std::move(executableMemory), std::move(readableMemory)), session, response);
        return {};
    }

    Result IJitEnvironment::GenerateCode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GenerateCode called (args=0x{:X}, input_buffers={}, output_buffers={}); compiler unavailable",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::Control(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: Control called (args=0x{:X}, input_buffers={}, output_buffers={}); compiler unavailable",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::LoadPlugin(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        // Actual command layout: u64 TransferMemorySize, one copied TransferMemory
        // handle, two input buffers (NRR followed by NRO).
        if (request.cmdArgSz < sizeof(u64) || request.copyHandles.size() != 1 || request.inputBuf.size() != 2) {
            LOGW("JIT_DIAG: LoadPlugin malformed IPC (args=0x{:X}, handles={}, buffers={})",
                 request.cmdArgSz, request.copyHandles.size(), request.inputBuf.size());
            return kernel::result::InvalidArgument;
        }

        const u64 workSize{request.Pop<u64>()};
        auto nrr{request.inputBuf[0]};
        auto nro{request.inputBuf[1]};
        LOGW("JIT_DIAG: LoadPlugin work_size=0x{:X}, NRR_size=0x{:X}, NRO_size=0x{:X}, handle=0x{:X}",
             workSize, nrr.size(), nro.size(), request.copyHandles[0]);

        std::shared_ptr<kernel::type::KTransferMemory> transferMemory;
        try {
            transferMemory = state.process->GetHandle<kernel::type::KTransferMemory>(request.copyHandles[0]);
        } catch (const std::exception &e) {
            LOGW("JIT_DIAG: LoadPlugin invalid TransferMemory handle: {}", e.what());
            return kernel::result::InvalidHandle;
        }

        if (!workSize || !util::IsPageAligned(workSize) || workSize > transferMemory->host.size()) {
            LOGW("JIT_DIAG: LoadPlugin invalid work_size 0x{:X} (backing=0x{:X})",
                 workSize, transferMemory->host.size());
            return kernel::result::InvalidSize;
        }

        if (nrr.size() < NrrHeaderSize || ReadU32(nrr, 0) != util::MakeMagic<u32>("NRR0")) {
            LOGW("JIT_DIAG: LoadPlugin NRR0 header missing or invalid");
            return kernel::result::InvalidArgument;
        }

        const u32 nrrSize{ReadU32(nrr, NrrSizeOffset)};
        const u32 hashOffset{ReadU32(nrr, NrrHashesOffset)};
        const u32 hashCount{ReadU32(nrr, NrrHashesCount)};
        const u8 nrrKind{nrr[NrrKindOffset]};
        if (nrrSize < NrrHeaderSize || nrrSize > nrr.size() ||
            nrrKind != 1 || hashOffset < NrrHeaderSize ||
            hashOffset > nrrSize || hashCount > (nrrSize - hashOffset) / HashSize) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRR layout size=0x{:X}, kind={}, hash_offset=0x{:X}, count={}",
                 nrrSize, nrrKind, hashOffset, hashCount);
            return kernel::result::InvalidArgument;
        }

        if (nro.size() < sizeof(loader::NroHeader)) {
            LOGW("JIT_DIAG: LoadPlugin NRO too small");
            return kernel::result::InvalidArgument;
        }

        loader::NroHeader header{};
        std::memcpy(&header, nro.data(), sizeof(header));
        if (header.magic != util::MakeMagic<u32>("NRO0") ||
            header.size < sizeof(header) || header.size > nro.size()) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRO header magic=0x{:X}, declared_size=0x{:X}",
                 header.magic, header.size);
            return kernel::result::InvalidArgument;
        }

        auto module{nro.subspan(0, header.size)};
        const auto segmentValid{[&](const loader::NroSegmentHeader &segment) {
            return ContainsBytes(module, segment.offset, segment.size);
        }};
        if (!segmentValid(header.text) || !segmentValid(header.ro) || !segmentValid(header.data) ||
            header.text.offset != 0 ||
            static_cast<u64>(header.text.offset) + header.text.size != header.ro.offset ||
            static_cast<u64>(header.ro.offset) + header.ro.size != header.data.offset ||
            static_cast<u64>(header.data.offset) + header.data.size != header.size) {
            LOGW("JIT_DIAG: LoadPlugin invalid NRO segment layout");
            return kernel::result::InvalidArgument;
        }

        std::array<u8, HashSize> nroHash{};
        if (mbedtls_sha256_ret(module.data(), module.size(), nroHash.data(), 0) != 0)
            return kernel::result::InvalidState;

        bool hashListed{};
        for (size_t i{}; i < hashCount; i++) {
            if (!std::memcmp(nrr.data() + hashOffset + i * HashSize, nroHash.data(), HashSize)) {
                hashListed = true;
                break;
            }
        }
        LOGW("JIT_DIAG: LoadPlugin NRR kind={}, hash_entries={}, NRO hash listed={}, segments text=0x{:X}, ro=0x{:X}, data=0x{:X}, bss=0x{:X}",
             nrrKind, hashCount, hashListed, header.text.size, header.ro.size, header.data.size, header.bssSize);

        if (!hashListed)
            return kernel::result::InvalidArgument;

        // Probe the dynamic symbol table without executing or relocating the NRO.
        // Checking inclusion in an NRR is NOT cryptographic signature verification.
        const auto dynamicSymbols{ResolveDynamicSymbols(module, header)};
        if (dynamicSymbols) {
            LOGW("JIT_DIAG: Plugin exports via {}: GetVersion={}, Configure={}, Control={}, GenerateCode={}, OnPrepared={}, ResolveBasicSymbols={}, SetupDiagnostics={}",
                 dynamicSymbols->source,
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginGetVersion"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginConfigure"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginControl"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginGenerateCode"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginOnPrepared"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginResolveBasicSymbols"),
                 HasSymbol(module, *dynamicSymbols, "nnjitpluginSetupDiagnostics"));
        } else {
            LOGW("JIT_DIAG: Plugin exports unavailable (no validated dynamic symbol table)");
        }

        LOGW("JIT_DIAG: NRR/NRO metadata validated; loading, relocation and guest plugin execution NOT implemented");
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::GetCodeAddress(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GetCodeAddress called; user CodeMemory aliases have not been mapped");
        return kernel::result::NotImplemented;
    }
}
