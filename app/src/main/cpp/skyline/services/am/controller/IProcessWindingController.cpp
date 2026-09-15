// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <services/am/applet/ILibraryAppletAccessor.h>
#include <services/am/storage/IStorage.h>
#include "IProcessWindingController.h"

namespace skyline::service::am {
    namespace {
        constexpr Result ObjectInvalid{128, 500};
    }

    IProcessWindingController::IProcessWindingController(const DeviceState &state, ServiceManager &manager,
                                                         std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IProcessWindingController::GetLaunchReason(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(0); // AppletProcessLaunchReason: no special launch flags.
        return {};
    }

    Result IProcessWindingController::OpenCallingLibraryApplet(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::shared_ptr<ILibraryAppletAccessor> accessor;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->reservedLibraryApplet) {
                accessor = std::move(appletState->reservedLibraryApplet);
                appletState->unwindAfterReserved = false;
            } else {
                accessor = appletState->callingLibraryApplet;
            }
        }
        if (!accessor)
            return result::NotAvailable;
        manager.RegisterService(accessor, session, response);
        return {};
    }

    Result IProcessWindingController::PushContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        std::scoped_lock lock{appletState->mutex};
        appletState->processWindingContext.emplace_back(std::move(storage));
        return {};
    }

    Result IProcessWindingController::PopContext(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->processWindingContext.empty())
                return result::NotAvailable;
            storage = std::move(appletState->processWindingContext.back());
            appletState->processWindingContext.pop_back();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IProcessWindingController::CancelWindingReservation(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->reservedLibraryApplet.reset();
        appletState->unwindAfterReserved = false;
        return {};
    }

    Result IProcessWindingController::WindAndDoReserved(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::shared_ptr<ILibraryAppletAccessor> accessor;
        {
            std::scoped_lock lock{appletState->mutex};
            accessor = appletState->reservedLibraryApplet;
            appletState->exitLocked = false;
        }
        if (!accessor)
            return result::NotAvailable;

        // Strato's library applets are frontend objects in the application process rather than
        // separate HOS processes, so start the reserved applet without terminating the caller.
        return accessor->StartApplet();
    }

    Result IProcessWindingController::ReserveToStartAndWaitAndUnwindThis(type::KSession &session,
                                                                         ipc::IpcRequest &request,
                                                                         ipc::IpcResponse &) {
        auto accessor{request.PopService<ILibraryAppletAccessor>(0, session)};
        if (!accessor)
            return ObjectInvalid;
        std::scoped_lock lock{appletState->mutex};
        appletState->reservedLibraryApplet = std::move(accessor);
        appletState->unwindAfterReserved = true;
        return {};
    }

    Result IProcessWindingController::ReserveToStartAndWait(type::KSession &session,
                                                            ipc::IpcRequest &request,
                                                            ipc::IpcResponse &) {
        auto accessor{request.PopService<ILibraryAppletAccessor>(0, session)};
        if (!accessor)
            return ObjectInvalid;
        std::scoped_lock lock{appletState->mutex};
        appletState->reservedLibraryApplet = std::move(accessor);
        appletState->unwindAfterReserved = false;
        return {};
    }
}
