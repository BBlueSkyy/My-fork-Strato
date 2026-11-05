// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <functional>
#include "../../fssrv/host/common.h"
namespace skyline {
    namespace kernel { class OS; }
    struct DeviceState { kernel::OS *os; };
    template<class T> span(std::vector<T>&) -> span<T>;
}
