// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <cstring>
#include <kernel/types/KProcess.h>
#include <nce/guest.h>
#include <os.h>
#include "symbol_hooks.h"
#include "symbol_hook_table.h"

namespace skyline::hle {
    void TraceCrysisAbortEntry(const DeviceState &state, const HookedSymbol &symbol) {
        if (!state.process || !state.thread || !state.process->is64bit() ||
            state.process->npdm.aci0.programId != 0x0100E66010ADE000ULL)
            return;

        const auto &guest{static_cast<const kernel::type::KNceThread &>(*state.thread).ctx};
        u64 callerLr{};
        if (guest.hostSp)
            std::memcpy(&callerLr, guest.hostSp, sizeof(callerLr));

        AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, fmt::format(
            "CrysisAbortEntry: {} callerLR=0x{:X} nzcv=0x{:08X} x0=0x{:X} x1=0x{:X} x2=0x{:X} x3=0x{:X} x4=0x{:X}",
            symbol.name, callerLr, guest.nzcv, guest.gpr.x0, guest.gpr.x1, guest.gpr.x2, guest.gpr.x3, guest.gpr.x4));

        // Compare bytes actually mapped for the current Program update with the
        // separately extracted base NSO. A nominal code-span address is not proof
        // that it is an ARM64 BL return site.
        if (callerLr >= 0x18 && (callerLr & 3) == 0) {
            std::string instructions;
            for (u64 address = callerLr - 0x18; address <= callerLr + 0x10; address += 4) {
                auto range{span<u8>{reinterpret_cast<u8 *>(address), sizeof(u32)}};
                if (!state.process->memory.AddressSpaceContains(range) ||
                    !state.process->memory.IsRangeMapped(range))
                    break;
                u32 word{};
                std::memcpy(&word, state.process->memory.TranslateVirtualPointer<const u8 *>(address), sizeof(word));
                instructions += fmt::format(" 0x{:X}={:08X}", address, word);
            }
            if (!instructions.empty())
                AsyncLogger::LogSync(AsyncLogger::LogLevel::Info,
                    fmt::format("CrysisAbortCode: LR=0x{:X}{}", callerLr, instructions));
        }

        // Only read the first argument as data, bounded and range-checked.
        // A string-looking pointer in the base NSO can address unrelated
        // bytes in the updated executable; don't assume x0 is a valid filename.
        if (guest.gpr.x0) {
            constexpr size_t ProbeLength{24};
            auto range{span<u8>{reinterpret_cast<u8 *>(guest.gpr.x0), ProbeLength}};
            if (state.process->memory.AddressSpaceContains(range) &&
                state.process->memory.IsRangeMapped(range)) {
                std::array<u8, ProbeLength> bytes{};
                std::memcpy(bytes.data(),
                            state.process->memory.TranslateVirtualPointer<const u8 *>(guest.gpr.x0),
                            bytes.size());
                std::string hexBytes;
                std::string printable;
                for (const auto byte : bytes) {
                    hexBytes += fmt::format("{:02X}", byte);
                    printable.push_back(byte >= 0x20 && byte <= 0x7E ? static_cast<char>(byte) : '.');
                }
                AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, fmt::format(
                    "CrysisAbortArg0: guest=0x{:X} first24={} ascii={}",
                    guest.gpr.x0, hexBytes, printable));
            }
        }
    }

    std::string Demangle(const std::string_view mangledName) {
        int status{};
        size_t length{};
        std::unique_ptr<char, decltype(&std::free)> demangled{abi::__cxa_demangle(mangledName.data(), nullptr, &length, &status), std::free};
        return std::string{status == 0 ? std::string_view{demangled.get()} : mangledName};
    }

    HookedSymbol::HookedSymbol(std::string pName, HookType hook) : name{std::move(pName)}, prettyName{Demangle(name)}, hook{std::move(hook)} {}

    HookedSymbolEntry::HookedSymbolEntry(std::string name, const HookType &hook, Elf64_Addr *offset) : HookedSymbol{std::move(name), hook}, offset{offset} {}

    std::vector<HookedSymbolEntry> GetExecutableSymbols(span<Elf64_Sym> dynsym, span<char> dynstr) {
        std::vector<HookedSymbolEntry> executableSymbols{};

        if constexpr (HookedSymbols.empty())
            return executableSymbols;

        for (auto &symbol : dynsym) {
            if (symbol.st_name == 0 || symbol.st_value == 0)
                continue;

            if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL || symbol.st_shndx == SHN_UNDEF)
                continue;

            std::string_view symbolName{dynstr.data() + symbol.st_name};

            auto item{std::find_if(HookedSymbols.begin(), HookedSymbols.end(), [&symbolName](const auto &item) {
                return item.name == symbolName;
            })};
            if (item != HookedSymbols.end()) {
                executableSymbols.emplace_back(std::string{symbolName}, item->hook, &symbol.st_value);
                continue;
            }

            #ifdef PRINT_HOOK_ALL
            if (symbolName == "memcpy" || symbolName == "memcmp" || symbolName == "memset" || symbolName == "strcmp" ||  symbolName == "strlen")
                // If symbol is from libc (such as memcpy, strcmp, strlen, etc), we don't need to hook it
                continue;

                executableSymbols.emplace_back(std::string{symbolName}, EntryExitHook{
                    .entry = [](const DeviceState &, const HookedSymbol &symbol) {
                        Logger::Debug("Entering \"{}\" ({})", symbol.prettyName, symbol.name);
                    },
                    .exit = [](const DeviceState &, const HookedSymbol &symbol) {
                        Logger::Debug("Exiting \"{}\"", symbol.prettyName);
                    },
                }, &symbol.st_value);
            #endif
        }

        return executableSymbols;
    }
}
