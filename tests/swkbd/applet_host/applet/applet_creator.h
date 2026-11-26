// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <services/am/applet/IApplet.h>
namespace skyline::applet {
    enum class AppletId : u32 { LibraryAppletSwkbd = 0x11 };
    std::shared_ptr<service::am::IApplet> CreateApplet(const DeviceState &, service::ServiceManager &, AppletId,
        std::shared_ptr<kernel::type::KEvent>, std::shared_ptr<kernel::type::KEvent>,
        std::shared_ptr<kernel::type::KEvent>, service::applet::LibraryAppletMode);
}
