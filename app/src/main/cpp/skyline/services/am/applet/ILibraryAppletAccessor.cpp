// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <applet/applet_creator.h>
#include <os.h>
#include <cstring>
#include <utility>
#include "AppletDataBroker.h"
#include "NativeAppletContext.h"
#include "ILibraryAppletAccessor.h"

namespace skyline::service::am {
    ILibraryAppletAccessor::ILibraryAppletAccessor(const DeviceState &state, ServiceManager &manager,
                                                   skyline::applet::AppletId appletId,
                                                   applet::LibraryAppletMode appletMode,
                                                   u64 appletResourceUserId)
        : BaseService(state, manager),
          stateChangeEvent(std::make_shared<type::KEvent>(state, false)),
          popNormalOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          popInteractiveOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          unknown170Event(std::make_shared<type::KEvent>(state, false)),
          appletId(appletId), appletMode(appletMode),
          applet(skyline::applet::CreateApplet(state, manager, appletId, stateChangeEvent,
                                               popNormalOutDataEvent, popInteractiveOutDataEvent,
                                               appletMode)), indirectLayers(manager.indirectLayers),
          appletResourceUserId(appletResourceUserId) {
        stateChangeEventHandle = state.process->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = state.process->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = state.process->InsertItem(popInteractiveOutDataEvent);
        LOGD("Applet accessor for {} ID created with appletMode 0x{:X}", ToString(appletId), appletMode);
    }

    ILibraryAppletAccessor::ILibraryAppletAccessor(
        const DeviceState &state, ServiceManager &manager, skyline::applet::AppletId appletId,
        applet::LibraryAppletMode appletMode, u64 appletResourceUserId,
        std::shared_ptr<NativeAppletContext> nativeContext, std::shared_ptr<AppletState> nativeAppletState)
        : BaseService(state, manager), unknown170Event(std::make_shared<type::KEvent>(state, false)),
          appletId(appletId), appletMode(appletMode), nativeContext(std::move(nativeContext)),
          nativeAppletState(std::move(nativeAppletState)), indirectLayers(manager.indirectLayers),
          appletResourceUserId(appletResourceUserId) {
        if (!this->nativeContext || !this->nativeContext->broker || !this->nativeContext->process ||
            !this->nativeContext->mainThread || !this->nativeAppletState)
            throw exception("Native library applet accessor requires a complete process context");

        stateChangeEvent = this->nativeContext->broker->stateChangedEvent;
        popNormalOutDataEvent = this->nativeContext->broker->NormalOutEvent();
        popInteractiveOutDataEvent = this->nativeContext->broker->InteractiveOutEvent();

        auto *callerProcess{state.GetCurrentProcessPtr()};
        stateChangeEventHandle = callerProcess->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = callerProcess->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = callerProcess->InsertItem(popInteractiveOutDataEvent);
        LOGI("Native applet accessor for {} prepared in guest process {}", ToString(appletId),
             this->nativeContext->process->id);
    }

    Result ILibraryAppletAccessor::CreateNative(
        const DeviceState &state, ServiceManager &manager, skyline::applet::AppletId appletId,
        applet::LibraryAppletMode appletMode, const std::shared_ptr<AppletState> &callerAppletState,
        std::shared_ptr<ILibraryAppletAccessor> &accessor) {
        if (!callerAppletState)
            return result::ObjectInvalid;

        constexpr u64 SwkbdProgramId{0x0100000000001008ULL};
        if (appletId != skyline::applet::AppletId::LibraryAppletSwkbd)
            return result::AppletLaunchFailed;

        auto loaded{state.os->LoadSystemProgram(SwkbdProgramId)};
        if (!loaded)
            return result::AppletLaunchFailed;

        auto context{std::make_shared<NativeAppletContext>()};
        context->broker = std::make_shared<AppletDataBroker>(state);
        context->process = std::move(loaded->process);
        context->mainThread = std::move(loaded->mainThread);
        context->appletId = static_cast<u32>(appletId);
        context->appletMode = static_cast<u32>(appletMode);
        context->desirableKeyboardLayout = callerAppletState->desirableKeyboardLayout;

        if (callerAppletState->nativeAppletContext) {
            context->callerAppletId = callerAppletState->nativeAppletContext->appletId;
            context->callerApplicationId = callerAppletState->nativeAppletContext->callerApplicationId;
        } else {
            context->callerAppletId = static_cast<u32>(skyline::applet::AppletId::Application);
            auto *callerProcess{state.GetCurrentProcessPtr()};
            if (!callerProcess)
                return result::AppletLaunchFailed;
            context->callerApplicationId = callerProcess->npdm.aci0.programId;
        }

        auto nativeAppletState{manager.GetOrCreateAppletState(context->process->id)};
        nativeAppletState->nativeAppletContext = context;
        nativeAppletState->desirableKeyboardLayout = context->desirableKeyboardLayout;

        accessor = std::shared_ptr<ILibraryAppletAccessor>(new ILibraryAppletAccessor(
            state, manager, appletId, appletMode, callerAppletState->appletResourceUserId,
            std::move(context), std::move(nativeAppletState)));
        return {};
    }

    ILibraryAppletAccessor::~ILibraryAppletAccessor() {
        indirectLayers->Unregister(indirectLayerHandle);
        if (nativeContext && nativeContext->started.load(std::memory_order_acquire) &&
            !nativeContext->exited.load(std::memory_order_acquire))
            nativeContext->process->Kill(false, true, true);
    }

    Result ILibraryAppletAccessor::StartApplet() {
        stateChangeEvent->ResetSignal();
        if (nativeContext) {
            if (!nativeContext->started.exchange(true, std::memory_order_acq_rel))
                nativeContext->mainThread->Start(false);
            return {};
        }
        return applet->Start();
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        if (nativeContext)
            return nativeContext->exited.load(std::memory_order_acquire);
        std::scoped_lock lock{kernel::type::KSyncObject::syncObjectMutex};
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(stateChangeEventHandle);
        LOGI("GetAppletStateChangedEvent: handle=0x{:X}, object={}", stateChangeEventHandle, fmt::ptr(stateChangeEvent.get()));
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const bool completed{IsAppletCompleted()};
        response.Push<u8>(completed);
        LOGI("IsCompleted: completed={}", completed);
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return StartApplet();
    }

    Result ILibraryAppletAccessor::RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        if (nativeContext) {
            nativeAppletState->QueueMessage(AppletState::ExitRequestedMessage);
            return {};
        }
        applet->RequestExit();
        indirectLayers->Unregister(indirectLayerHandle);
        indirectLayerHandle = 0;
        exited = true;
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        if (nativeContext) {
            nativeContext->terminated.store(true, std::memory_order_release);
            nativeContext->process->Kill(false, true, true);
            nativeContext->exited.store(true, std::memory_order_release);
            stateChangeEvent->Signal();
            return {};
        }
        applet->RequestExit();
        indirectLayers->Unregister(indirectLayerHandle);
        indirectLayerHandle = 0;
        exited = true;
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::GetResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        if (nativeContext) {
            if (!nativeContext->started.load(std::memory_order_acquire) ||
                nativeContext->terminated.load(std::memory_order_acquire))
                return result::LibraryAppletTerminated;
            return {};
        }
        return applet->GetResult();
    }

    Result ILibraryAppletAccessor::PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        if (nativeContext)
            nativeContext->broker->PushNormalIn(std::move(storage));
        else
            applet->PushNormalDataToApplet(std::move(storage));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto data{request.PopService<IStorage>(0, session)};
        const auto dataSpan{data->GetSpan()};
        if (dataSpan.size() >= sizeof(u32)) {
            u32 command{};
            std::memcpy(&command, dataSpan.data(), sizeof(command));
            LOGI("PushInteractiveInData: command=0x{:X}, size=0x{:X}", command, dataSpan.size());
        } else {
            LOGI("PushInteractiveInData: command=<truncated>, size=0x{:X}", dataSpan.size());
        }
        if (nativeContext)
            nativeContext->broker->PushInteractiveIn(std::move(data));
        else
            applet->PushInteractiveDataToApplet(std::move(data));
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto outStorage{nativeContext ? nativeContext->broker->PopNormalOut() : applet->PopNormalAndClear()};
        if (outStorage) {
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::PopInteractiveOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto outStorage{nativeContext ? nativeContext->broker->PopInteractiveOut() : applet->PopInteractiveAndClear()};
        if (outStorage) {
            manager.RegisterService(outStorage, session, response);
            LOGI("PopInteractiveOutData: Success");
            return {};
        }
        LOGI("PopInteractiveOutData: NotAvailable");
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        LOGI("GetPopInteractiveOutDataEvent: handle=0x{:X}, object={}", popInteractiveOutDataEventHandle,
             fmt::ptr(popInteractiveOutDataEvent.get()));
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (nativeContext)
            return result::ObjectInvalid;
        const auto requestedAppletResourceUserId{request.Pop<u64>()};
        if (exited || appletMode != applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !request.pid ||
            requestedAppletResourceUserId != appletResourceUserId)
            return result::ObjectInvalid;
        if (!indirectLayerHandle ||
            !indirectLayers->Get(indirectLayerHandle, request.pid, requestedAppletResourceUserId)) {
            indirectLayers->Unregister(indirectLayerHandle);
            indirectLayerHandle = indirectLayers->Register(applet, request.pid, requestedAppletResourceUserId);
        }
        response.Push<u64>(indirectLayerHandle);
        LOGI("GetIndirectLayerConsumerHandle: success, handle=0x{:X}, pid=0x{:X}, ARUID=0x{:X}",
             indirectLayerHandle, request.pid, requestedAppletResourceUserId);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto *process{nativeContext ? state.GetCurrentProcessPtr() : state.process.get()};
        response.copyHandles.push_back(process->InsertItem(unknown170Event));
        return {};
    }
}
