// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "INgcServiceForApplication.h"

namespace skyline::service::ngc {
    INgcServiceForApplication::INgcServiceForApplication(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}
}
