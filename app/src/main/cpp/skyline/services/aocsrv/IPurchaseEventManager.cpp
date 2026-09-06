// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <array>
#include <kernel/types/KProcess.h>
#include "IPurchaseEventManager.h"

namespace skyline::service::aocsrv {
    IPurchaseEventManager::IPurchaseEventManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          purchasedEvent(std::make_shared<type::KEvent>(state, false)) {}

    Result IPurchaseEventManager::SetDefaultDeliveryTarget(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IPurchaseEventManager::GetPurchasedEventReadableHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(purchasedEvent)};
        LOGD("Purchased Event Readable Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }

    Result IPurchaseEventManager::PopPurchasedProductInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // nn::ec::detail::PurchasedProductInfo is 0x80 bytes. We don't emulate eShop
        // purchases, but callers still expect the full response payload when this stub
        // reports success. Returning Success without the payload causes titles such as
        // Dying Light to repeatedly poll this command.
        std::array<u8, 0x80> purchasedProductInfo{};
        response.Push(purchasedProductInfo);

        return {};
    }
}
