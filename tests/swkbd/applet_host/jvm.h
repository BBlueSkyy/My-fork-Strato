// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <test_host.h>
#include <skyline/applet/swkbd/software_keyboard_frontend.h>
namespace skyline {
    class JvmManager {
      public:
        applet::swkbd::FrontendSessionRegistry sessions;
        applet::swkbd::FrontendSessionId current{};
        unsigned opens{}, hides{}, closes{}, updates{}, retries{}, checks{};
        bool inlineMode{};
        applet::swkbd::FrontendSessionId CreateSoftwareKeyboardSession(std::weak_ptr<applet::swkbd::SoftwareKeyboardFrontendCallbacks> callbacks) {
            return current = sessions.Register(std::move(callbacks));
        }
        bool ShowSoftwareKeyboard(applet::swkbd::FrontendSessionId, const applet::swkbd::FrontendKeyboardConfig &, std::u16string_view, bool inlineKeyboard) {
            ++opens; inlineMode = inlineKeyboard; return true;
        }
        void ShowSoftwareKeyboardTextCheck(applet::swkbd::FrontendSessionId, u32, std::u16string_view) { ++checks; }
        void ResumeSoftwareKeyboard(applet::swkbd::FrontendSessionId) { ++retries; }
        void UpdateSoftwareKeyboard(applet::swkbd::FrontendSessionId, std::u16string_view, i32) { ++updates; }
        void HideSoftwareKeyboard(applet::swkbd::FrontendSessionId) { ++hides; }
        void CloseSoftwareKeyboardSession(applet::swkbd::FrontendSessionId id) { ++closes; sessions.Unregister(id); }
        void Event(applet::swkbd::FrontendEventType type, std::u16string text = {}, i32 cursor = 0) {
            if (!sessions.Dispatch({current, type, std::move(text), cursor}))
                throw exception("Unexpected stale frontend event in fixture");
        }
    };
}
