// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <array>
#include <common.h>
#include <mutex>
#include <unordered_map>

namespace skyline::applet::swkbd {
    using FrontendSessionId = u64;
    using FrontendKeyboardConfig = std::array<u8, 0x4C8>;

    enum class FrontendEventType : u32 {
        TextChanged = 0,
        Submit = 1,
        Cancel = 2,
        TextCheckAccepted = 3,
        TextCheckDismissed = 4,
        FrontendDestroyed = 5,
    };

    struct FrontendEvent {
        FrontendSessionId sessionId{};
        FrontendEventType type{};
        std::u16string text;
        i32 cursor{};
    };

    class SoftwareKeyboardFrontendCallbacks {
      public:
        virtual ~SoftwareKeyboardFrontendCallbacks() = default;
        virtual void OnSoftwareKeyboardFrontendEvent(FrontendEvent event) = 0;
    };

    class FrontendSessionRegistry {
      private:
        std::mutex mutex;
        FrontendSessionId nextSessionId{1};
        std::unordered_map<FrontendSessionId, std::weak_ptr<SoftwareKeyboardFrontendCallbacks>> sessions;

      public:
        FrontendSessionId Register(std::weak_ptr<SoftwareKeyboardFrontendCallbacks> callbacks);
        bool Unregister(FrontendSessionId sessionId);
        bool Dispatch(FrontendEvent event);
    };
}