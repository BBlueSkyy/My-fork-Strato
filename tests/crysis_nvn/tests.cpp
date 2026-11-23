// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "nce/crysis_nvn_probe.h"
namespace probe = skyline::nce::crysis;
int main() {
    // Fails if another title/build, a shortened SDK or one altered instruction
    // can receive executable diagnostic patches, or installation partly mutates it.
    const std::array<std::uint8_t, 32> sdkId{
        0x2F,0x08,0x86,0x2A,0x30,0x91,0x1F,0x87,0xA1,0x3B,0x11,0xF7,0xDD,0x0F,0x50,0x9D,
        0xCB,0x42,0x2D,0x2D};
    std::vector<std::uint8_t> image(0x45B130);
    const std::array<std::pair<std::size_t, std::array<std::uint32_t,4>>,4> entries{{
        {0x45AD80,{0xF81B0FFA,0xA90163F9,0xA9025BF7,0xA90353F5}},
        {0x45AF40,{0xA9BE53F5,0xA9017BF3,0xF9402C08,0xAA0203F5}},
        {0x45B080,{0xA9BF7BF3,0xF9400C08,0xF9001408,0x320003EA}},
        {0x45B120,{0xD10103FF,0xA9015BF7,0xA90253F5,0xA9037BF3}},
    }};
    for (const auto &[offset, words] : entries)
        std::memcpy(image.data()+offset,words.data(),sizeof(words));
    const auto original=image;
    auto badId=sdkId;badId[31]=1;
    assert(!probe::InstallSdkProbes(image,0,"sdk.nso",sdkId));assert(image==original);
    assert(!probe::InstallSdkProbes(image,0x0100E66010ADE000ULL,"main.nso",sdkId));assert(image==original);
    assert(!probe::InstallSdkProbes(image,0x0100E66010ADE000ULL,"sdk.nso",badId));assert(image==original);
    image[0x45B12C]^=1;
    auto damaged=image;
    assert(!probe::InstallSdkProbes(image,0x0100E66010ADE000ULL,"sdk.nso",sdkId));assert(image==damaged);
    image=original;image.resize(0x45B12F);auto shortened=image;
    assert(!probe::InstallSdkProbes(image,0x0100E66010ADE000ULL,"sdk.nso",sdkId));assert(image==shortened);
    image=original;
    assert(probe::InstallSdkProbes(image,0x0100E66010ADE000ULL,"sdk.nso",sdkId));
    auto expected=original;
    for(std::size_t i=0;i<entries.size();++i) {
        const std::uint32_t svc=0xD4000001U | ((0x7FF1U+i)<<5);
        std::memcpy(expected.data()+entries[i].first,&svc,4);
        assert(probe::ReplayInstruction(0x7FF1+i)==entries[i].second[0]);
    }
    assert(image==expected);
    assert(probe::ReplayInstruction(0x7FF0)==0xF81F0FFE);
    assert(probe::ReplayInstruction(0x7FEF)==0);
    assert(probe::ReplayInstruction(0x7FF5)==0);
    // Usable size includes subtraction of the reserved trailer, without
    // conflating configured size with usable size or underflowing bad cursors.
    std::array<std::uint64_t,15> raw{};
    raw[0x50/8]=0x100000;raw[0x18/8]=0xCFFF48;raw[0x20/8]=0xCFFF80;
    raw[0x58/8]=0x2000000;raw[0x30/8]=0x2000040;raw[0x38/8]=0x207FFF0;
    raw[0x70/8]=32;
    auto state=probe::Decode(raw);
    assert(state.command.valid && state.command.size==0xBFFF80);
    assert(state.command.used==0xBFFF48 && state.command.free==56);
    assert(state.control.valid && state.control.size==0x80000);
    assert(state.control.used==64 && state.control.free==0x7FFC0);
    raw[0x18/8]=0xCFFF84;assert(!probe::Decode(raw).command.valid);
    raw[0x18/8]=0xFFFFF;assert(!probe::Decode(raw).command.valid);
    raw[0x38/8]=UINT64_MAX;assert(!probe::Decode(raw).control.valid);
    std::cout << "Crysis NVN identity, atomic installation, replay and accounting: PASS\n";
}
