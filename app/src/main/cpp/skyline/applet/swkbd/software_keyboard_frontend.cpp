// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#include "software_keyboard_frontend.h"

namespace skyline::applet::swkbd {
    FrontendSessionId FrontendSessionRegistry::Register(std::weak_ptr<SoftwareKeyboardFrontendCallbacks> callbacks) {
        std::scoped_lock lock{mutex};
        if (!nextSessionId)
            throw exception("Software keyboard frontend session IDs exhausted");
        const FrontendSessionId sessionId{nextSessionId++};
        sessions.emplace(sessionId, std::move(callbacks));
        return sessionId;
    }

    bool FrontendSessionRegistry::Unregister(FrontendSessionId sessionId) {
        std::scoped_lock lock{mutex};
        return sessions.erase(sessionId) != 0;
    }

    bool FrontendSessionRegistry::Dispatch(FrontendEvent event) {
        std::shared_ptr<SoftwareKeyboardFrontendCallbacks> callbacks;
        {
            std::scoped_lock lock{mutex};
            const auto entry{sessions.find(event.sessionId)};
            if (entry == sessions.end())
                return false;
            callbacks = entry->second.lock();
            if (!callbacks) {
                sessions.erase(entry);
                return false;
            }
        }
        callbacks->OnSoftwareKeyboardFrontendEvent(std::move(event));
        return true;
    }
}