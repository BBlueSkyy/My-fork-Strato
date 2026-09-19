// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace skyline::service::am {
    class IApplet;

    // Shared by AM accessors and VI sessions. Entries do not extend an applet's lifetime.
    class IndirectLayerRegistry {
      private:
        struct Entry {
            std::weak_ptr<IApplet> applet;
            std::uint64_t processId{};
            std::uint64_t appletResourceUserId{};
        };

        std::mutex mutex;
        std::uint64_t nextHandle{1};
        std::unordered_map<std::uint64_t, Entry> applets;

      public:
        std::uint64_t Register(const std::shared_ptr<IApplet> &applet, std::uint64_t processId,
                               std::uint64_t appletResourceUserId) {
            std::scoped_lock lock{mutex};
            if (!nextHandle)
                throw std::overflow_error("Indirect layer handles exhausted");
            const auto handle{nextHandle++};
            applets.emplace(handle, Entry{applet, processId, appletResourceUserId});
            return handle;
        }

        std::shared_ptr<IApplet> Get(std::uint64_t handle, std::uint64_t processId,
                                     std::uint64_t appletResourceUserId) {
            std::scoped_lock lock{mutex};
            const auto entry{applets.find(handle)};
            if (entry == applets.end() || entry->second.processId != processId ||
                entry->second.appletResourceUserId != appletResourceUserId)
                return nullptr;
            auto applet{entry->second.applet.lock()};
            if (!applet)
                applets.erase(entry);
            return applet;
        }

        void Unregister(std::uint64_t handle) {
            std::scoped_lock lock{mutex};
            applets.erase(handle);
        }
    };
}