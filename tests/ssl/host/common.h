// SPDX-License-Identifier: MPL-2.0
// Minimal host boundary used to compile the production SSL service sources.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <common/result.h>

#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)

namespace skyline {
    template<typename T>
    using span = std::span<T>;

    struct HostOS {
        std::string publicAppFilesPath;
        std::string privateAppFilesPath;
    };

    struct DeviceState {
        HostOS *os;
    };
}
