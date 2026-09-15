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
        std::mutex mutex;
        std::uint64_t nextHandle{1};
        std::unordered_map<std::uint64_t, std::weak_ptr<IApplet>> applets;

      public:
        std::uint64_t Register(const std::shared_ptr<IApplet> &applet) {
            std::scoped_lock lock{mutex};
            if (!nextHandle)
                throw std::overflow_error("Indirect layer handles exhausted");
            const auto handle{nextHandle++};
            applets.emplace(handle, applet);
            return handle;
        }

        std::shared_ptr<IApplet> Get(std::uint64_t handle) {
            std::scoped_lock lock{mutex};
            const auto entry{applets.find(handle)};
            return entry == applets.end() ? nullptr : entry->second.lock();
        }

        void Unregister(std::uint64_t handle) {
            std::scoped_lock lock{mutex};
            applets.erase(handle);
        }
    };
}
