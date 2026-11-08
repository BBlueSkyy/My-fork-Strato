// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <common/result.h>
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
namespace skyline {
    template<class T> using span = std::span<T>;
    struct HostOS { std::string publicAppFilesPath; std::string privateAppFilesPath; };
    struct DeviceState { HostOS *os{}; };
}
