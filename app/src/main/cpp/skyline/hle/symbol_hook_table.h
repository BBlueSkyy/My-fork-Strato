// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "symbol_hooks.h"

namespace skyline::hle {
    struct HookTableEntry {
        std::string_view name; //!< The name of the symbol
        HookType hook; //!< The hook that the symbol should include

        HookTableEntry(std::string_view name, HookType hook) : name{name}, hook{std::move(hook)} {}
    };

    // These entries are considered only for Crysis Remastered's sdk.nso by Loader.
    // Entry+exit (rather than OverrideHook) preserves the original guest function.
    static const std::array<HookTableEntry, 8> HookedSymbols{{
        {"_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_z",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iS3_z",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"nndetailDiagAbortImpl",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"nndetailDiagOnAssertionFailureFormat",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
        {"_ZN2nn4diag6detail22InvokeSdkAbortObserverERKNS0_12SdkAbortInfoE",
            EntryExitHook{.entry = TraceCrysisAbortEntry, .exit = [](const DeviceState &, const HookedSymbol &) {}}},
    }};
}
