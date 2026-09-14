// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::service::ipc {
struct IpcRequest {
    u8 *cmdArg{};
    u64 cmdArgSz{};
    std::vector<span<u8>> inputBuf, outputBuf;
};
struct IpcResponse {
    std::vector<u8> payload;
    template<class T> void Push(const T &value) {
        auto start = reinterpret_cast<const u8 *>(&value);
        payload.insert(payload.end(), start, start + sizeof(T));
    }
};
}
