// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/deadcells_vk_identity_format.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
using namespace skyline::gpu::deadcells_probe::vk_link;
int main() {
    auto name = Name("queue", 128, 0, 0x1817120000ULL, 0xB400006F82042488ULL, 2784);
    assert(name.find("role=queue") != std::string::npos);
    assert(name.find("frame=128") != std::string::npos);
    assert(name.find("guest=0x1817120000") != std::string::npos);
    assert(name.find("nvmap=2784") != std::string::npos);
    assert(name.find("host=0xB400006F82042488") != std::string::npos);
    assert(HandleBits(std::uint64_t{0xDEADBEEF01234567ULL}) == 0xDEADBEEF01234567ULL);
    auto ptr = reinterpret_cast<void *>(std::uintptr_t{0x12345});
    assert(HandleBits(ptr) == 0x12345ULL);
    std::puts("Dead Cells VkImage names: PASS");
}
