// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include "AppletDataBroker.h"
#include <services/am/storage/IStorage.h>

namespace skyline::service::am {
    AppletDataBroker::AppletDataBroker(const DeviceState &state)
        : normalIn(state), interactiveIn(state), normalOut(state), interactiveOut(state),
          stateChangedEvent(std::make_shared<kernel::type::KEvent>(state, false)) {}

    void AppletDataBroker::Push(Channel &channel, std::shared_ptr<IStorage> storage) {
        std::scoped_lock lock{channel.mutex};
        channel.queue.emplace_back(std::move(storage));
        channel.event->Signal();
    }

    std::shared_ptr<IStorage> AppletDataBroker::Pop(Channel &channel) {
        std::scoped_lock lock{channel.mutex};
        if (channel.queue.empty())
            return nullptr;
        auto storage{std::move(channel.queue.front())};
        channel.queue.pop_front();
        if (channel.queue.empty())
            channel.event->ResetSignal();
        return storage;
    }

    void AppletDataBroker::PushNormalIn(std::shared_ptr<IStorage> storage) { Push(normalIn, std::move(storage)); }
    void AppletDataBroker::PushInteractiveIn(std::shared_ptr<IStorage> storage) { Push(interactiveIn, std::move(storage)); }
    void AppletDataBroker::PushNormalOut(std::shared_ptr<IStorage> storage) { Push(normalOut, std::move(storage)); }
    void AppletDataBroker::PushInteractiveOut(std::shared_ptr<IStorage> storage) { Push(interactiveOut, std::move(storage)); }

    std::shared_ptr<IStorage> AppletDataBroker::PopNormalIn() { return Pop(normalIn); }
    std::shared_ptr<IStorage> AppletDataBroker::PopInteractiveIn() { return Pop(interactiveIn); }
    std::shared_ptr<IStorage> AppletDataBroker::PopNormalOut() { return Pop(normalOut); }
    std::shared_ptr<IStorage> AppletDataBroker::PopInteractiveOut() { return Pop(interactiveOut); }
}
