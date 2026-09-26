// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <os.h>
#include <kernel/types/KProcess.h>
#include "IAddOnContentManager.h"
#include "IPurchaseEventManager.h"

namespace skyline::service::aocsrv {
    IAddOnContentManager::IAddOnContentManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          addOnContentListChangedEvent(std::make_shared<type::KEvent>(state, false)) {}

    Result IAddOnContentManager::CountAddOnContent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGI("AOC-TRACE CountAddOnContent loaders={}", state.dlcLoaders.size());
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

        const size_t outputBufferSize{request.outputBuf.empty() ? 0 : request.outputBuf.at(0).size()};
        LOGI("AOC-TRACE ListAddOnContent offset={} count={} loaders={} outbuf={}",
             params.offset, params.count, state.dlcLoaders.size(), outputBufferSize);

        std::vector<u32> out;
        std::vector<u64> aocTitleIds;

        for (u32 i = 0; i < state.dlcLoaders.size(); i++)
            aocTitleIds.push_back(state.dlcLoaders[i]->cnmt->header.id);

        for (u64 contentId : aocTitleIds)
            out.push_back(static_cast<u32>(contentId & constant::AOCTitleIdMask));

        LOGI("AOC-TRACE ListAddOnContent built={} offset={} count={} outbuf={}",
             out.size(), params.offset, params.count, outputBufferSize);

        const auto outCount{static_cast<u32>(std::min<size_t>(out.size() - params.offset, params.count))};
        std::rotate(out.begin(), out.begin() + params.offset, out.end());
        out.resize(outCount);

        request.outputBuf.at(0).copy_from(out);
        LOGI("AOC-TRACE ListAddOnContent returning outCount={}", outCount);
        response.Push<u32>(outCount);
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
