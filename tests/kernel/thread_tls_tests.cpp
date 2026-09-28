// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cassert>
#include <cstring>
#include <kernel/thread_tls.h>

int main() {
    using namespace skyline;
    std::array<u8, 0x400> tls{};
    tls.fill(0xA5);

    const KHandle first{0xD001};
    const KHandle second{0xD002};
    kernel::WriteCurrentThreadHandle(tls.data(), first);
    kernel::WriteCurrentThreadHandle(tls.data() + 0x200, second);

    KHandle storedFirst{}, storedSecond{};
    std::memcpy(&storedFirst, tls.data() + 0x110, sizeof(storedFirst));
    std::memcpy(&storedSecond, tls.data() + 0x310, sizeof(storedSecond));
    assert(storedFirst == first);
    assert(storedSecond == second);
    assert(storedFirst != storedSecond);
    assert(tls[0x10F] == 0xA5);
    assert(tls[0x114] == 0xA5);
    assert(tls[0x30F] == 0xA5);
    assert(tls[0x314] == 0xA5);
}
