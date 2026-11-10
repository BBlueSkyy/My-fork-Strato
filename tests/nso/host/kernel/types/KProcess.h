// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::memory { enum class AddressSpaceType { AddressSpace39Bit }; }
namespace skyline::kernel::type {
struct KProcess {
    struct { struct { u64 programId{}; } aci0; struct { struct { bool is64Bit{}; } flags; } meta; } npdm;
    struct {
        void InitializeVmm(memory::AddressSpaceType) {}
        void InitializeRegions(span<u8>) {}
    } memory;
};
}
