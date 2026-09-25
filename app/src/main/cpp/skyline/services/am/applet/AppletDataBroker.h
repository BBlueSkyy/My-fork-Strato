// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <kernel/types/KEvent.h>

namespace skyline::service::am {
    class IStorage;

    /**
     * @brief Bidirectional storage/event bridge between a LibraryApplet caller and a guest applet process.
     */
    class AppletDataBroker {
      private:
        struct Channel {
            std::mutex mutex;
            std::deque<std::shared_ptr<IStorage>> queue;
            std::shared_ptr<kernel::type::KEvent> event;

            explicit Channel(const DeviceState &state)
                : event(std::make_shared<kernel::type::KEvent>(state, false)) {}
        };

        Channel normalIn;
        Channel interactiveIn;
        Channel normalOut;
        Channel interactiveOut;

        static void Push(Channel &channel, std::shared_ptr<IStorage> storage);
        static void PushFront(Channel &channel, std::shared_ptr<IStorage> storage);
        static std::shared_ptr<IStorage> Pop(Channel &channel);

      public:
        explicit AppletDataBroker(const DeviceState &state);

        std::shared_ptr<kernel::type::KEvent> stateChangedEvent;

        void PushNormalIn(std::shared_ptr<IStorage> storage);
        void PushFrontNormalIn(std::shared_ptr<IStorage> storage);
        void PushInteractiveIn(std::shared_ptr<IStorage> storage);
        void PushNormalOut(std::shared_ptr<IStorage> storage);
        void PushInteractiveOut(std::shared_ptr<IStorage> storage);

        std::shared_ptr<IStorage> PopNormalIn();
        std::shared_ptr<IStorage> PopInteractiveIn();
        std::shared_ptr<IStorage> PopNormalOut();
        std::shared_ptr<IStorage> PopInteractiveOut();

        const std::shared_ptr<kernel::type::KEvent> &NormalInEvent() const { return normalIn.event; }
        const std::shared_ptr<kernel::type::KEvent> &InteractiveInEvent() const { return interactiveIn.event; }
        const std::shared_ptr<kernel::type::KEvent> &NormalOutEvent() const { return normalOut.event; }
        const std::shared_ptr<kernel::type::KEvent> &InteractiveOutEvent() const { return interactiveOut.event; }
    };
}
