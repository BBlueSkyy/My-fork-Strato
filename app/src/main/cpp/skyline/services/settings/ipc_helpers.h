// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <algorithm>
#include <cstring>
#include <services/base_service.h>
#include <kernel/results.h>

namespace skyline::service::settings {
    namespace result {
        constexpr Result InvalidLanguage{105, 625};
        constexpr Result NullFirmwareBuffer{105, 1141};
        constexpr Result InvalidKeyboardLayout{105, 1245};
        // CMIF's unknown method result; do not fall through BaseService's Success fallback.
        constexpr Result UnknownCommand{10, 221};
    }

    template<typename T>
    Result WriteBuffer(ipc::IpcRequest &request, const T &value, Result nullResult = kernel::result::InvalidArgument) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (request.outputBuf.empty() || !request.outputBuf[0].data())
            return nullResult;
        if (request.outputBuf[0].size_bytes() < sizeof(T))
            return kernel::result::InvalidArgument;
        std::memcpy(request.outputBuf[0].data(), &value, sizeof(T));
        return {};
    }

    template<typename T>
    ResultValue<T> ReadBuffer(ipc::IpcRequest &request) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (request.inputBuf.empty() || !request.inputBuf[0].data() || request.inputBuf[0].size_bytes() < sizeof(T))
            return kernel::result::InvalidArgument;
        T value;
        std::memcpy(&value, request.inputBuf[0].data(), sizeof(T));
        return value;
    }

    template<typename T>
    ResultValue<T> ReadArgument(ipc::IpcRequest &request) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!request.cmdArg || request.cmdArgSz < sizeof(T))
            return kernel::result::InvalidArgument;
        T value;
        std::memcpy(&value, request.cmdArg, sizeof(T));
        return value;
    }

    template<typename T>
    Result PushValue(ipc::IpcResponse &response, ResultValue<T> value) {
        if (value)
            response.Push(*value);
        return value.result;
    }
}
