// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/uuid.h>
#include <cstring>
#include <limits>
#include <mbedtls/sha1.h>
#include <loader/loader.h>
#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include <jvm.h>
#include <nce.h>
#include <os.h>
#include <services/account/IAccountServiceForApplication.h>
#include <services/am/storage/VectorIStorage.h>
#include <services/fssrv/IFileSystemProxy.h>
#include <services/fssrv/results.h>
#include "IApplicationFunctions.h"

namespace skyline::service::am {
    IApplicationFunctions::IApplicationFunctions(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          gpuErrorEvent(std::make_shared<type::KEvent>(state, false)),
          friendInvitationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
          notificationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
          unknownEvent210(std::make_shared<type::KEvent>(state, false)) {}

    Result IApplicationFunctions::PopLaunchParameter(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        constexpr u32 LaunchParameterMagic{0xC79497CA}; //!< The magic of the application launch parameters
        constexpr size_t LaunchParameterSize{0x88}; //!< The size of the launch parameter IStorage

        enum class LaunchParameterKind : u32 {
            UserChannel = 1,
            PreselectedUser = 2,
            Unknown = 3,
        } launchParameterKind{request.Pop<LaunchParameterKind>()};

        std::shared_ptr<IStorage> storageService;
        switch (launchParameterKind) {
            case LaunchParameterKind::UserChannel: {
                LOGI("Multiprogram trace: PopLaunchParameter(UserChannel) begin current={} previous={}",
                     state.os->GetCurrentProgramIndex(), state.os->GetPreviousProgramIndex());
                auto data{state.os->PopUserChannel()};
                if (!data) {
                    LOGI("Multiprogram trace: PopLaunchParameter(UserChannel) -> NotAvailable");
                    return result::NotAvailable;
                }
                LOGI("Multiprogram trace: PopLaunchParameter(UserChannel) -> {} bytes", data->size());
                storageService = std::make_shared<VectorIStorage>(state, manager, std::move(*data));
                break;
            }

            case LaunchParameterKind::PreselectedUser: {
                storageService = std::make_shared<VectorIStorage>(state, manager, LaunchParameterSize);

                storageService->Push<u32>(LaunchParameterMagic);
                storageService->Push<u32>(1);
                storageService->Push(constant::DefaultUserId);

                break;
            }

            case LaunchParameterKind::Unknown:
                throw exception("Popping 'Unknown' Launch Parameter: {}", static_cast<u32>(launchParameterKind));

            default:
                return result::InvalidInput;
        }

        manager.RegisterService(storageService, session, response);
        return {};
    }

    Result IApplicationFunctions::EnsureSaveData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto userId{request.Pop<account::UserId>()};
        const auto &nacp{state.loader->nacp->nacpContents};

        const auto ensureResult{fssrv::EnsureApplicationSaveData(state.os->publicAppFilesPath,
                                                                 nacp.saveDataOwnerId,
                                                                 userId,
                                                                 nacp.userAccountSaveDataSize,
                                                                 nacp.userAccountSaveDataJournalSize,
                                                                 nacp.deviceSaveDataSize,
                                                                 nacp.deviceSaveDataJournalSize)};
        if (ensureResult)
            return ensureResult;

        response.Push<u64>(0);
        return {};
    }

    Result IApplicationFunctions::SetTerminateResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto result{request.Pop<Result>()};
        LOGI("App set termination result: {} (module {}, description {})", result.raw,
             static_cast<u32>(result.module), static_cast<u32>(result.id));
        if (!state.process->is64bit()) {
            const auto &guest{static_cast<const kernel::type::KJit32Thread &>(*state.thread).ctx};
            LOGI("Guest AArch32 termination call: PC=0x{:X}, LR=0x{:X}, SP=0x{:X}",
                 guest.pc, guest.lr, guest.sp);
        }
        return {};
    }

    Result IApplicationFunctions::GetDesiredLanguage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto desiredLanguage{language::GetApplicationLanguage(*state.settings->systemLanguage)};

        // In the future we might want to trigger an UI dialog if the user-selected language is not available, for now it will use the first one available
        if (((1 << static_cast<u32>(desiredLanguage)) & state.loader->nacp->nacpContents.supportedLanguageFlag) == 0)
            desiredLanguage = state.loader->nacp->GetFirstSupportedLanguage();

        response.Push(language::GetLanguageCode(language::GetSystemLanguage(desiredLanguage)));
        return {};
    }

    Result IApplicationFunctions::GetDisplayVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(state.loader->nacp->nacpContents.displayVersion);
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto saveDataType{request.Pop<u64>()};
        auto userId{request.Pop<account::UserId>()};
        LOGD("Save data type: {}, UserId: {:016X}{:016X}", saveDataType, userId.upper, userId.lower);

        response.Push(SaveDataSize);
        response.Push(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::CreateCacheStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        struct CreateCacheStorageInput {
            u64 index;
            i64 saveSize;
            i64 journalSize;
        };
        static_assert(sizeof(CreateCacheStorageInput) == 0x18);

        if (!request.cmdArg || request.cmdArgSz < sizeof(CreateCacheStorageInput))
            return fssrv::result::InvalidArgument;
        CreateCacheStorageInput input{};
        std::memcpy(&input, request.cmdArg, sizeof(input));

        const auto &nacp{state.loader->nacp->nacpContents};

        fssrv::CacheStorageTargetMedia targetMedia{};
        u64 requiredSize{};
        const auto createResult{fssrv::CreateApplicationCacheStorage(
                state.os->publicAppFilesPath,
                nacp.saveDataOwnerId,
                nacp.cacheStorageIndexMax,
                nacp.cacheStorageDataAndJournalSizeMax,
                input.index,
                input.saveSize,
                input.journalSize,
                targetMedia,
                requiredSize)};
        if (createResult)
            return createResult;

        response.Push<u64>(static_cast<u64>(targetMedia));
        response.Push(requiredSize);
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSizeMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(SaveDataSize);
        response.Push(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::GetCacheStorageMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        struct CacheStorageMaxResponse {
            i32 indexMax;
            u32 padding;
            i64 dataAndJournalSizeMax;
        };
        static_assert(sizeof(CacheStorageMaxResponse) == 0x10);

        const auto &nacp{state.loader->nacp->nacpContents};
        const CacheStorageMaxResponse max{
            static_cast<i32>(nacp.cacheStorageIndexMax),
            0,
            static_cast<i64>(nacp.cacheStorageDataAndJournalSizeMax)
        };

        LOGD("Cache storage max: index={}, data+journal=0x{:X}", max.indexMax, max.dataAndJournalSizeMax);
        response.Push(max);
        return {};
    }

    Result IApplicationFunctions::NotifyRunning(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(1);
        return {};
    }

    Result IApplicationFunctions::GetPseudoDeviceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto seedForPseudoDeviceId{state.loader->nacp->nacpContents.seedForPseudoDeviceId};
        std::array<u8, 20> hashBuf{};

        // On HOS the seed from control.ncap is hashed together with the device specific device ID seed
        // for us it's enough to just hash the seed from control.nacp as it provides the same guarantees
        if (int err{mbedtls_sha1_ret(seedForPseudoDeviceId.data(), seedForPseudoDeviceId.size(), hashBuf.data())}; err < 0)
            throw exception("Failed to hash device ID, err: {}", err);

        response.Push<UUID>(UUID::GenerateUuidV5(hashBuf));
        return {};
    }

    Result IApplicationFunctions::InitializeGamePlayRecording(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::SetGamePlayRecordingState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::EnableApplicationCrashReport(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 width{request.Pop<i32>()};
        i32 height{request.Pop<i32>()};
        u64 transferMemorySize{request.Pop<u64>()};

        constexpr i32 MaximumFbWidth{1280};
        constexpr i32 MaximumFbHeight{720};
        constexpr u64 RequiredFbAlignment{0x40000};

        if (width > MaximumFbWidth || height > MaximumFbHeight || !util::IsAligned(transferMemorySize, RequiredFbAlignment))
            return result::InvalidParameters;

        LOGD("Dimensions: ({}, {}) Transfer Memory Size: {}", width, height, transferMemorySize);

        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightImage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 x{request.Pop<i32>()};
        i32 y{request.Pop<i32>()};
        i32 width{request.Pop<i32>()};
        i32 height{request.Pop<i32>()};

        enum class WindowOriginMode : i32 {
            LowerLeft,
            UpperLeft
        } originMode = request.Pop<WindowOriginMode>();

        if (y < 0 || x < 0 || width < 1 || height < 1)
            return result::InvalidParameters;

        LOGD("Position: ({}, {}) Dimensions: ({}, {}) Origin mode: {}", x, y, width, height, static_cast<i32>(originMode));
        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightVisibility(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u8 visiblity{request.Pop<u8>()};
        LOGD("Visiblity: {}", visiblity);
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatistics(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatisticsByUid(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result IApplicationFunctions::ExecuteProgram(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGI("Multiprogram trace: ExecuteProgram IPC entered cmdArgSz={} current={} previous={}",
             request.cmdArgSz, state.os->GetCurrentProgramIndex(), state.os->GetPreviousProgramIndex());

        enum class ProgramSpecifyKind : u32 {
            ExecuteProgram = 0,
            JumpToSubApplicationProgramForDevelopment = 1,
            RestartProgram = 2,
        };

        struct ExecuteProgramInput {
            ProgramSpecifyKind kind;
            u32 padding;
            u64 value;
        };
        static_assert(sizeof(ExecuteProgramInput) == 0x10);

        if (!request.cmdArg || request.cmdArgSz < sizeof(ExecuteProgramInput))
            return result::InvalidInput;

        ExecuteProgramInput input{};
        std::memcpy(&input, request.cmdArg, sizeof(input));

        u8 programIndex{};
        switch (input.kind) {
            case ProgramSpecifyKind::ExecuteProgram:
                if (input.value > std::numeric_limits<u8>::max())
                    return result::InvalidInput;
                programIndex = static_cast<u8>(input.value);
                break;
            case ProgramSpecifyKind::RestartProgram:
                if (input.value != 0)
                    return result::InvalidInput;
                programIndex = state.os->GetCurrentProgramIndex();
                break;
            case ProgramSpecifyKind::JumpToSubApplicationProgramForDevelopment:
            default:
                return result::InvalidInput;
        }

        const auto userChannel{state.os->GetUserChannelSnapshot()};
        std::vector<u8> serialized;
        size_t totalSize{sizeof(u32)};
        for (const auto &entry : userChannel) {
            if (entry.size() > std::numeric_limits<u32>::max())
                return result::InvalidInput;
            totalSize += sizeof(u32) + entry.size();
        }
        serialized.reserve(totalSize);

        auto pushU32{[&serialized](u32 value) {
            serialized.push_back(static_cast<u8>(value));
            serialized.push_back(static_cast<u8>(value >> 8));
            serialized.push_back(static_cast<u8>(value >> 16));
            serialized.push_back(static_cast<u8>(value >> 24));
        }};
        pushU32(static_cast<u32>(userChannel.size()));
        for (const auto &entry : userChannel) {
            pushU32(static_cast<u32>(entry.size()));
            serialized.insert(serialized.end(), entry.begin(), entry.end());
        }

        LOGI("ExecuteProgram: current ProgramIndex {}, target ProgramIndex {}",
             state.os->GetCurrentProgramIndex(), programIndex);
        if (!state.jvm->RequestProgramRelaunch(static_cast<i32>(input.kind), input.value,
                                               programIndex, state.os->GetCurrentProgramIndex(), serialized))
            throw exception("Android frontend rejected Program relaunch request");

        // The Android relaunch trampoline owns termination from this point onward. Exit the
        // requesting guest process path while the trampoline captures the last presented frame.
        throw nce::NCE::ExitException(true);
    }

    Result IApplicationFunctions::ClearUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGI("Multiprogram trace: ClearUserChannel current={} previous={}",
             state.os->GetCurrentProgramIndex(), state.os->GetPreviousProgramIndex());
        state.os->ClearUserChannel();
        return {};
    }

    Result IApplicationFunctions::UnpopToUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGI("Multiprogram trace: UnpopToUserChannel begin current={} previous={}",
             state.os->GetCurrentProgramIndex(), state.os->GetPreviousProgramIndex());
        auto storage{request.PopService<IStorage>(0, session)};
        if (!storage) {
            LOGI("Multiprogram trace: UnpopToUserChannel -> InvalidInput (missing storage)");
            return result::InvalidInput;
        }

        auto data{storage->GetSpan()};
        LOGI("Multiprogram trace: UnpopToUserChannel push {} bytes", data.size());
        state.os->PushUserChannel(std::vector<u8>{data.begin(), data.end()});
        return {};
    }

    Result IApplicationFunctions::GetPreviousProgramIndex(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const auto previous{state.os->GetPreviousProgramIndex()};
        LOGI("Multiprogram trace: GetPreviousProgramIndex -> {}", previous);
        response.Push<i32>(previous);
        return {};
    }

    Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(gpuErrorEvent)};
        LOGD("GPU Error Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(friendInvitationStorageChannelEvent)};
        LOGD("Friend Invitiation Storage Channel Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return result::NotAvailable;
    }

    Result IApplicationFunctions::GetNotificationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(notificationStorageChannelEvent)};
        LOGW("Notification Storage Channel Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::Cmd210(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Undocumented cmd added in FW 20.0.0, no input/output bytes, just an output handle.
        // We stub it out the same way as the other unimplemented system events: hand back a
        // valid handle to an event that is never signalled, so games that query it don't crash
        // on the missing HIPC function while waiting on an event that (as far as we know) is
        // tied to some system-level condition we don't emulate.
        auto handle{state.process->InsertItem(unknownEvent210)};
        LOGW("Stubbed cmd 210 (unknown system event) Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }
}
