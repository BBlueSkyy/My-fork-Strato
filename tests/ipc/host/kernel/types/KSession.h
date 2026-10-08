// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::kernel::type {
    struct KSession {
        bool isDomain{};
        std::map<u32,std::shared_ptr<service::BaseService>> domains;
        std::shared_ptr<service::BaseService> serviceObject;
        DeviceState state;
    };
}
