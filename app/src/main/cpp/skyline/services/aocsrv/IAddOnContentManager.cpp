// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <os.h>
#include <kernel/types/KProcess.h>
#include "IAddOnContentManager.h"
#include "IPurchaseEventManager.h"

namespace skyline::service::aocsrv {
    IAddOnContentManager::IAddOnContentManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          addOnContentListChangedEvent(std::make_shared<type::KEvent>(state, false)) {}

    Result IAddOnContentManager::CountAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(state.dlcLoaders.size()));
        return {};
    }

    Result IAddOnContentManager::ListAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        struct Parameters {
            u32 offset;
            u32 count;
            u64 processId;
        };
        auto params{request.Pop<Parameters>()};

        std::vector<u32> out;
        out.reserve(state.dlcLoaders.size());
        for (const auto &dlc : state.dlcLoaders) {
            if (dlc && dlc->cnmt)
                out.push_back(static_cast<u32>(dlc->cnmt->header.id & constant::AOCTitleIdMask));
        }

        // The guest may request a page starting after the last available DLC.
        // Clamp to the guest's output buffer as well, avoiding invalid iterators
        // and never announcing content that was rejected during startup.
        const size_t outputCapacity{request.outputBuf.empty() ? 0 : request.outputBuf[0].size() / sizeof(u32)};
        const size_t offset{std::min<size_t>(params.offset, out.size())};
        const size_t outCount{std::min({out.size() - offset, static_cast<size_t>(params.count), outputCapacity})};
        if (outCount) {
            std::vector<u32> page(out.begin() + offset, out.begin() + offset + outCount);
            request.outputBuf[0].copy_from(page);
        }
        response.Push<u32>(static_cast<u32>(outCount));
        return {};
    }

    Result IAddOnContentManager::GetAddOnContentBaseId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(state.loader->nacp->nacpContents.addOnContentBaseId);
        return {};
    }

    Result IAddOnContentManager::PrepareAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IAddOnContentManager::GetAddOnContentListChangedEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        KHandle handle{state.process->InsertItem(addOnContentListChangedEvent)};
        LOGD("Add On Content List Changed Event Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }

    Result IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        KHandle handle{state.process->InsertItem(addOnContentListChangedEvent)};
        LOGD("Add On Content List Changed Event Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }

    Result IAddOnContentManager::CheckAddOnContentMountStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IAddOnContentManager::CreateEcPurchasedEventManager(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IPurchaseEventManager), session, response);
        return {};
    }
}
