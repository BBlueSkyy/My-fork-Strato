// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::ngc {
    /**
     * @brief INgcServiceForApplication provides access to NG (content/word) filtering functionality
     * @note Unofficial name; minimal stub so the service can be created instead of throwing when
     *       requested — individual commands fall back to BaseService's unimplemented-function path
     */
    class INgcServiceForApplication : public BaseService {
      public:
        INgcServiceForApplication(const DeviceState &state, ServiceManager &manager);
    };
}
