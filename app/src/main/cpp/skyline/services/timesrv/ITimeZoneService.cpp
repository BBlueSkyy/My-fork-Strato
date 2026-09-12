// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <os.h>
#include <kernel/types/KProcess.h>
#include "common.h"
#include "core.h"
#include "ITimeZoneService.h"

namespace skyline::service::timesrv {
    namespace {
        ResultValue<std::string> NormalizeLocationName(const LocationName &raw) {
            const auto end{std::find(raw.begin(), raw.end(), '\0')};
            if (end == raw.begin() || end == raw.end())
                return result::InvalidArgument;
            return std::string(raw.begin(), end);
        }

        LocationName MakeLocationName(std::string_view name) {
            LocationName out{};
            const auto length{std::min(name.size(), out.size() - 1)};
            std::memcpy(out.data(), name.data(), length);
            return out;
        }
    }

    ITimeZoneService::ITimeZoneService(const DeviceState &state, ServiceManager &manager, core::TimeServiceObject &core, bool writeable)
        : BaseService(state, manager),
          core(core),
          writeable(writeable) {}

    Result ITimeZoneService::GetDeviceLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto locationName{core.timeZoneManager.GetLocationName()};
        if (locationName)
            response.Push(*locationName);

        return locationName;
    }

    Result ITimeZoneService::SetDeviceLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!writeable)
            return result::PermissionDenied;

        const auto rawName{request.Pop<LocationName>()};
        auto locationName{NormalizeLocationName(rawName)};
        if (!locationName)
            return locationName.result;

        const auto normalized{MakeLocationName(*locationName)};
        if (std::find(core.locationNameList.begin(), core.locationNameList.end(), normalized) == core.locationNameList.end())
            return result::TimeZoneNotFound;

        auto file{state.os->assetFileSystem->OpenFileUnchecked(fmt::format("tzdata/zoneinfo/{}", *locationName))};
        if (!file || !file->size || file->size > 0x100000)
            return result::TimeZoneNotFound;

        std::vector<u8> binary(file->size);
        if (file->ReadUnchecked(binary) != binary.size())
            return result::RuleConversionFailed;

        return SetDeviceLocationNameWithTimeZoneBinary(*locationName, binary);
    }

    Result ITimeZoneService::GetTotalLocationNameCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto count{core.timeZoneManager.GetLocationCount()};
        if (count)
            response.Push<u32>(static_cast<u32>(*count));

        return count;
    }

    Result ITimeZoneService::LoadLocationNameList(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto offset{request.Pop<u32>()};
        if (request.outputBuf.empty())
            return result::InvalidArgument;

        auto outList{request.outputBuf.at(0).cast<LocationName>()};
        const size_t start{std::min<size_t>(offset, core.locationNameList.size())};
        const size_t count{std::min(outList.size(), core.locationNameList.size() - start)};
        for (size_t i{}; i < count; ++i)
            outList[i] = core.locationNameList[start + i];

        response.Push<u32>(static_cast<u32>(count));
        return {};
    }

    Result ITimeZoneService::LoadTimeZoneRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.outputBuf.empty())
            return result::InvalidArgument;

        const auto rawName{request.Pop<LocationName>()};
        auto locationName{NormalizeLocationName(rawName)};
        if (!locationName)
            return locationName.result;

        const auto normalized{MakeLocationName(*locationName)};
        if (std::find(core.locationNameList.begin(), core.locationNameList.end(), normalized) == core.locationNameList.end())
            return result::TimeZoneNotFound;

        auto file{state.os->assetFileSystem->OpenFileUnchecked(fmt::format("tzdata/zoneinfo/{}", *locationName))};
        if (!file || !file->size || file->size > 0x100000)
            return result::TimeZoneNotFound;

        std::vector<u8> binary(file->size);
        if (file->ReadUnchecked(binary) != binary.size())
            return result::RuleConversionFailed;

        return core::TimeZoneManager::ParseTimeZoneBinary(binary, request.outputBuf.at(0));
    }

    Result ITimeZoneService::GetTimeZoneRuleVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto version{core.timeZoneManager.GetBinaryVersion()};
        if (version)
            response.Push(*version);

        return version;
    }

    Result ITimeZoneService::GetDeviceLocationNameAndUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto locationName{core.timeZoneManager.GetLocationName()};
        if (!locationName)
            return locationName;

        auto updateTime{core.timeZoneManager.GetUpdateTime()};
        if (!updateTime)
            return updateTime;

        response.Push(*locationName);
        response.Push<u32>(0); // CMIF padding before SteadyClockTimePoint.
        response.Push(*updateTime);
        return {};
    }

    Result ITimeZoneService::SetDeviceLocationNameWithTimeZoneBinaryIpc(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.inputBuf.empty())
            return result::InvalidArgument;

        const auto rawName{request.Pop<LocationName>()};
        auto locationName{NormalizeLocationName(rawName)};
        if (!locationName)
            return locationName.result;

        return SetDeviceLocationNameWithTimeZoneBinary(*locationName, request.inputBuf.at(0));
    }

    Result ITimeZoneService::SetDeviceLocationNameWithTimeZoneBinary(std::string_view locationName, span<u8> binary) {
        if (!writeable)
            return result::PermissionDenied;

        auto result{core.timeZoneManager.SetNewLocation(locationName, binary)};
        if (result)
            return result;

        auto timePoint{core.standardSteadyClock.GetCurrentTimePoint()};
        if (!timePoint)
            return timePoint;

        core.timeZoneManager.SetUpdateTime(*timePoint);
        return {};
    }

    Result ITimeZoneService::ParseTimeZoneBinaryIpc(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.inputBuf.empty() || request.outputBuf.empty())
            return result::InvalidArgument;
        return core::TimeZoneManager::ParseTimeZoneBinary(request.inputBuf.at(0), request.outputBuf.at(0));
    }

    Result ITimeZoneService::ParseTimeZoneBinary(span<u8> binary, span<u8> rule) {
        return core::TimeZoneManager::ParseTimeZoneBinary(binary, rule);
    }

    Result ITimeZoneService::GetDeviceLocationNameOperationEventReadableHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!operationEvent) {
            operationEvent = std::make_shared<kernel::type::KEvent>(state, false);
            core.timeZoneManager.AddOperationEvent(operationEvent);
        }

        response.copyHandles.push_back(state.process->InsertItem(operationEvent));
        return {};
    }

    Result ITimeZoneService::ToCalendarTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.inputBuf.empty())
            return result::InvalidArgument;

        auto posixTime{request.Pop<PosixTime>()};
        auto calendarTime{core::TimeZoneManager::ToCalendarTime(reinterpret_cast<tz_timezone_t>(request.inputBuf.at(0).data()), posixTime)};

        if (calendarTime)
            response.Push(*calendarTime);

        return calendarTime;
    }

    Result ITimeZoneService::ToCalendarTimeWithMyRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto posixTime{request.Pop<PosixTime>()};
        auto calendarTime{core.timeZoneManager.ToCalendarTimeWithMyRule(posixTime)};

        if (calendarTime)
            response.Push(*calendarTime);

        return calendarTime;
    }

    Result ITimeZoneService::ToPosixTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.inputBuf.empty() || request.outputBuf.empty() || request.outputBuf.at(0).size_bytes() < sizeof(PosixTime))
            return result::InvalidArgument;

        auto calendarTime{request.Pop<CalendarTime>()};
        auto posixTimes{core::TimeZoneManager::ToPosixTime(reinterpret_cast<tz_timezone_t>(request.inputBuf.at(0).data()), calendarTime)};
        if (!posixTimes)
            return posixTimes;

        auto output{request.outputBuf.at(0).cast<PosixTime>()};
        const size_t count{std::min(output.size(), posixTimes->size())};
        for (size_t i{}; i < count; ++i)
            output[i] = posixTimes->at(i);

        response.Push<u32>(static_cast<u32>(count));
        return {};
    }

    Result ITimeZoneService::ToPosixTimeWithMyRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.outputBuf.empty() || request.outputBuf.at(0).size_bytes() < sizeof(PosixTime))
            return result::InvalidArgument;

        auto calendarTime{request.Pop<CalendarTime>()};
        auto posixTimes{core.timeZoneManager.ToPosixTimeWithMyRule(calendarTime)};
        if (!posixTimes)
            return posixTimes;

        auto output{request.outputBuf.at(0).cast<PosixTime>()};
        const size_t count{std::min(output.size(), posixTimes->size())};
        for (size_t i{}; i < count; ++i)
            output[i] = posixTimes->at(i);

        response.Push<u32>(static_cast<u32>(count));
        return {};
    }
}
