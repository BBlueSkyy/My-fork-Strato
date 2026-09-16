// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <common.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace skyline::service::am {
    class IApplet;

    /**
     * @brief Associates VI indirect-layer consumer handles with the applet that owns them.
     * @note Entries use weak ownership so VI cannot extend an applet's lifetime.
     */
    class IndirectLayerRegistry {
      private:
        struct Entry {
            std::weak_ptr<IApplet> applet;
            u64 appletResourceUserId;
        };

        std::mutex mutex;
        u64 nextHandle{1};
        std::unordered_map<u64, Entry> entries;

      public:
        u64 Register(const std::shared_ptr<IApplet> &applet, u64 appletResourceUserId) {
            std::scoped_lock lock{mutex};
            if (!nextHandle)
                throw exception("Indirect layer handle space exhausted");

            const u64 handle{nextHandle++};
            entries.emplace(handle, Entry{applet, appletResourceUserId});
            return handle;
        }

        std::shared_ptr<IApplet> Get(u64 handle, u64 appletResourceUserId) {
            std::scoped_lock lock{mutex};
            auto it{entries.find(handle)};
            if (it == entries.end() || it->second.appletResourceUserId != appletResourceUserId)
                return {};

            auto applet{it->second.applet.lock()};
            if (!applet)
                entries.erase(it);
            return applet;
        }

        void Unregister(u64 handle) {
            if (!handle)
                return;
            std::scoped_lock lock{mutex};
            entries.erase(handle);
        }
    };
}
