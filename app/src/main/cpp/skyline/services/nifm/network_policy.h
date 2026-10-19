// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <sys/socket.h>

namespace skyline::service::nifm {
    // NIFM GetInternetConnectionStatus always returns a 3-byte status payload.
    // 0 = not connected; 4 = connected. An absent connection is not an IPC error.
    struct InternetConnectionStatus {
        std::uint8_t type;         // NetworkInterfaceType::Wifi
        std::uint8_t wifiStrength; // 0 when offline
        std::uint8_t state;        // 0 when offline; 4 when connected
    };
    static_assert(sizeof(InternetConnectionStatus) == 3);

    constexpr InternetConnectionStatus MakeInternetConnectionStatus(bool connected) {
        return {1, static_cast<std::uint8_t>(connected ? 3 : 0),
                   static_cast<std::uint8_t>(connected ? 4 : 0)};
    }

    // Mirror Eden's connected socket classification. Datagram sockets can
    // still be created locally; BSD denies their external sends while offline.
    constexpr bool IsConnectionOrientedSocket(int type) {
        return type == SOCK_STREAM || type == SOCK_SEQPACKET;
    }
}
