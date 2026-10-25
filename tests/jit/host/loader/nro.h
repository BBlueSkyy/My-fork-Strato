// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::loader {
    struct NroSegmentHeader {
        u32 offset;
        u32 size;
    };

    struct NroHeader {
        u32 _pad0_;
        u32 modOffset; //!< The offset of the MOD metadata
        u64 _pad1_;

        u32 magic; //!< The NRO magic "NRO0"
        u32 version; //!< The version of the application
        u32 size; //!< The size of the NRO
        u32 flags; //!< The flags used with the NRO

        NroSegmentHeader text; //!< The .text segment header
        NroSegmentHeader ro; //!< The .rodata segment header
        NroSegmentHeader data; //!< The .data segment header

        u32 bssSize; //!< The size of the bss segment
        u32 _pad2_;
        std::array<u64, 4> buildId; //!< The build ID of the NRO
        u64 _pad3_;

        NroSegmentHeader apiInfo; //!< The .apiInfo segment header
        NroSegmentHeader dynstr; //!< The .dynstr segment header
        NroSegmentHeader dynsym; //!< The .dynsym segment header
    };

}
