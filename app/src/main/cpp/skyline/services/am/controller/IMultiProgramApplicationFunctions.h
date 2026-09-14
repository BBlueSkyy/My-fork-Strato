// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/signal.h>
#include <os.h>
#include <services/account/IAccountServiceForApplication.h>
#include <services/am/storage/VectorIStorage.h>
#include "IApplicationFunctions.h"

namespace skyline::service::am {
    /**
     * Minimal multi-program extension for IApplicationFunctions.
     *
     * This deliberately keeps the scheduler/kernel/NCE implementation from PR #146 untouched.
     * Commands unrelated to multi-program are delegated to the original IApplicationFunctions.
     */
    class IMultiProgramApplicationFunctions : public IApplicationFunctions {
      private:
        std::vector<std::shared_ptr<IStorage>> userChannel;
        i32 previousProgramIndex{-1};

        Result PopLaunchParameterMultiProgram(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
            constexpr u32 LaunchParameterMagic{0xC79497CA};
            constexpr size_t LaunchParameterSize{0x88};

            enum class LaunchParameterKind : u32 {
                UserChannel = 1,
                PreselectedUser = 2,
                Unknown = 3,
            };

            const auto kind{request.Pop<LaunchParameterKind>()};
            std::shared_ptr<IStorage> storage;

            switch (kind) {
                case LaunchParameterKind::UserChannel:
                    if (userChannel.empty())
                        return result::NotAvailable;
                    storage = std::move(userChannel.back());
                    userChannel.pop_back();
                    break;

                case LaunchParameterKind::PreselectedUser:
                    storage = std::make_shared<VectorIStorage>(state, manager, LaunchParameterSize);
                    storage->Push<u32>(LaunchParameterMagic);
                    storage->Push<u32>(1);
                    storage->Push(constant::DefaultUserId);
                    break;

                case LaunchParameterKind::Unknown:
                    return result::NotAvailable;

                default:
                    return result::InvalidInput;
            }

            manager.RegisterService(std::move(storage), session, response);
            return {};
        }

        Result ExecuteProgram(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
            enum class ProgramSpecifyKind : u32 {
                ExecuteProgram = 0,
                SubApplicationProgram = 1,
                RestartProgram = 2,
            };

            const auto kind{request.Pop<ProgramSpecifyKind>()};
            request.Skip<u32>();
            const u64 value{request.Pop<u64>()};

            u8 targetProgramIndex{};
            switch (kind) {
                case ProgramSpecifyKind::ExecuteProgram:
                    if (value > 0xFF)
                        return result::InvalidParameters;
                    targetProgramIndex = static_cast<u8>(value);
                    break;

                case ProgramSpecifyKind::RestartProgram:
                    if (value != 0)
                        return result::InvalidParameters;
                    targetProgramIndex = state.os->GetCurrentProgramIndex();
                    break;

                case ProgramSpecifyKind::SubApplicationProgram:
                default:
                    return result::InvalidInput;
            }

            std::vector<std::vector<u8>> serializedUserChannel;
            serializedUserChannel.reserve(userChannel.size());
            for (const auto &storage : userChannel) {
                auto data{storage->GetSpan()};
                serializedUserChannel.emplace_back(data.begin(), data.end());
            }
            userChannel.clear();

            LOGI("ExecuteProgram: kind={} value={} current={} target={}", static_cast<u32>(kind), value,
                 state.os->GetCurrentProgramIndex(), targetProgramIndex);
            state.os->RequestProgramExecution(targetProgramIndex, std::move(serializedUserChannel));

            // ExecuteProgram replaces the current application. Commit the successful response to
            // TLS before leaving HOS-1, then let PR #146's native SignalException -> KThread cleanup
            // path return control to OS::Execute. Do not call KProcess::Kill from this HOS thread.
            response.errorCode = {};
            response.WriteResponse(session.isDomain);
            signal::SignalException exitSignal;
            exitSignal.signal = SIGINT;
            throw exitSignal;
        }

        Result ClearUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
            userChannel.clear();
            return {};
        }

        Result UnpopToUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
            userChannel.emplace_back(request.PopService<IStorage>(0, session));
            return {};
        }

        Result GetPreviousProgramIndexMultiProgram(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
            response.Push<i32>(previousProgramIndex);
            return {};
        }

      protected:
        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override {
            if (!isTipc) {
                using Function = Result (IMultiProgramApplicationFunctions::*)(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
                Function function{};
                const char *name{};

                switch (id) {
                    case 1:
                        function = &IMultiProgramApplicationFunctions::PopLaunchParameterMultiProgram;
                        name = "IMultiProgramApplicationFunctions::PopLaunchParameter";
                        break;
                    case 120:
                        function = &IMultiProgramApplicationFunctions::ExecuteProgram;
                        name = "IMultiProgramApplicationFunctions::ExecuteProgram";
                        break;
                    case 121:
                        function = &IMultiProgramApplicationFunctions::ClearUserChannel;
                        name = "IMultiProgramApplicationFunctions::ClearUserChannel";
                        break;
                    case 122:
                        function = &IMultiProgramApplicationFunctions::UnpopToUserChannel;
                        name = "IMultiProgramApplicationFunctions::UnpopToUserChannel";
                        break;
                    case 123:
                        function = &IMultiProgramApplicationFunctions::GetPreviousProgramIndexMultiProgram;
                        name = "IMultiProgramApplicationFunctions::GetPreviousProgramIndex";
                        break;
                    default:
                        return IApplicationFunctions::GetServiceFunction(id, isTipc);
                }

                return ServiceFunctionDescriptor{
                    reinterpret_cast<DerivedService *>(this),
                    reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(function),
                    name,
                };
            }

            return IApplicationFunctions::GetServiceFunction(id, isTipc);
        }

      public:
        IMultiProgramApplicationFunctions(const DeviceState &state, ServiceManager &manager)
            : IApplicationFunctions(state, manager), previousProgramIndex(state.os->GetPreviousProgramIndex()) {
            auto launchParameters{state.os->TakeUserChannelLaunchParameters()};
            userChannel.reserve(launchParameters.size());
            for (auto &parameter : launchParameters)
                userChannel.emplace_back(std::make_shared<VectorIStorage>(state, manager, std::move(parameter)));
        }
    };
}
