// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::memory {
enum class AddressSpaceType : u8 {
    AddressSpace32Bit = 0, AddressSpace36Bit = 1,
    AddressSpace32BitNoReserved = 2, AddressSpace39Bit = 3,
};
}
