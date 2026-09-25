// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <atomic>
#include <memory>

namespace skyline::kernel::type {
    class KProcess;
    class KThread;
}

namespace skyline::service::am {
    class AppletDataBroker;

    struct NativeAppletContext {
        std::shared_ptr<AppletDataBroker> broker;
        std::shared_ptr<kernel::type::KProcess> process;
        std::shared_ptr<kernel::type::KThread> mainThread;
        u32 appletId{};
        u32 appletMode{};
        u32 callerAppletId{};
        u32 desirableKeyboardLayout{};
        u64 callerApplicationId{};
        std::atomic_bool started{};
        std::atomic_bool exited{};
    };
}
