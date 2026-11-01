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
        // All registrations are Crysis-specific, but guard this entry point too.
        if (!state.process || !state.thread || !state.process->is64bit() ||
            state.process->npdm.aci0.programId != 0x0100E66010ADE000ULL)
            return;

        const auto &guest{static_cast<const kernel::type::KNceThread &>(*state.thread).ctx};
        u64 callerLr{};
        // EntryExitHook stores the original guest LR in a dedicated 16-byte
        // slot referenced by hostSp, before saving the volatile guest registers.
        if (guest.hostSp)
            std::memcpy(&callerLr, guest.hostSp, sizeof(callerLr));
        AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, fmt::format(
            "CrysisAbortEntry: {} callerLR=0x{:X} x0=0x{:X} x1=0x{:X} x2=0x{:X} x3=0x{:X} x4=0x{:X}",
            symbol.name, callerLr, guest.gpr.x0, guest.gpr.x1, guest.gpr.x2, guest.gpr.x3, guest.gpr.x4));
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
