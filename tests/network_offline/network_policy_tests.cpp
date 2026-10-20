// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <services/nifm/network_policy.h>

using namespace skyline::service::nifm;

int main() {
    constexpr auto offline = MakeInternetConnectionStatus(false);
    constexpr auto online = MakeInternetConnectionStatus(true);
    static_assert(sizeof(offline) == 3);
    static_assert(offline.type == 1 && offline.wifiStrength == 0 && offline.state == 0);
    static_assert(online.type == 1 && online.wifiStrength == 3 && online.state == 4);
    static_assert(IsConnectionOrientedSocket(SOCK_STREAM));
    static_assert(IsConnectionOrientedSocket(SOCK_SEQPACKET));
    static_assert(!IsConnectionOrientedSocket(SOCK_DGRAM));
    assert(offline.state != online.state);
}
