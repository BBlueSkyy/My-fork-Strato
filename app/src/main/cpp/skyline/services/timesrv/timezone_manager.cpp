// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "results.h"
#include "timezone_manager.h"

namespace skyline::service::timesrv::core {
    TimeZoneManager::~TimeZoneManager() {
        std::scoped_lock lock{mutex};
        if (rule)
            tz_tzfree(rule);
    }

    void TimeZoneManager::SignalOperationEventsLocked() {
        for (auto it{operationEvents.begin()}; it != operationEvents.end();) {
            if (auto event{it->lock()}) {
                event->Signal();
                ++it;
            } else {
                it = operationEvents.erase(it);
            }
        }
    }

    Result TimeZoneManager::Setup(std::string_view pLocationName, const SteadyClockTimePoint &pUpdateTime, int pLocationCount, std::array<u8, 0x10> pBinaryVersion, span<u8> binary) {
        auto result{SetNewLocation(pLocationName, binary)};
        if (result)
            return result;

        SetUpdateTime(pUpdateTime);
        SetLocationCount(pLocationCount);
        SetBinaryVersion(pBinaryVersion);

        MarkInitialized();
        return {};
    }

    ResultValue<LocationName> TimeZoneManager::GetLocationName() {
        std::scoped_lock lock{mutex};

        if (!IsInitialized())
            return result::ClockUninitialized;

        return locationName;
    }

    Result TimeZoneManager::SetNewLocation(std::string_view pLocationName, span<u8> binary) {
        const auto nul{pLocationName.find('\0')};
        if (nul != std::string_view::npos)
            pLocationName = pLocationName.substr(0, nul);

        if (pLocationName.empty() || pLocationName.size() >= LocationName{}.size() || binary.empty())
            return result::InvalidArgument;

        auto newRule{tz_tzalloc(binary.data(), static_cast<long>(binary.size()))};
        if (!newRule)
            return result::RuleConversionFailed;

        std::scoped_lock lock{mutex};
        if (rule)
            tz_tzfree(rule);
        rule = newRule;

        locationName.fill(0);
        std::memcpy(locationName.data(), pLocationName.data(), pLocationName.size());
        return {};
    }

    ResultValue<SteadyClockTimePoint> TimeZoneManager::GetUpdateTime() {
        std::scoped_lock lock{mutex};

        if (!IsInitialized())
            return result::ClockUninitialized;

        return updateTime;
    }

    void TimeZoneManager::SetUpdateTime(const SteadyClockTimePoint &pUpdateTime) {
        std::scoped_lock lock{mutex};
        if (updateTime == pUpdateTime)
            return;

        updateTime = pUpdateTime;
        SignalOperationEventsLocked();
    }

    ResultValue<int> TimeZoneManager::GetLocationCount() {
        std::scoped_lock lock{mutex};

        if (!IsInitialized())
            return result::ClockUninitialized;

        return locationCount;
    }

    void TimeZoneManager::SetLocationCount(int pLocationCount) {
        std::scoped_lock lock{mutex};
        locationCount = pLocationCount;
    }

    ResultValue<std::array<u8, 0x10>> TimeZoneManager::GetBinaryVersion() {
        std::scoped_lock lock{mutex};

        if (!IsInitialized())
            return result::ClockUninitialized;

        return binaryVersion;
    }

    void TimeZoneManager::SetBinaryVersion(std::array<u8, 0x10> pBinaryVersion) {
        std::scoped_lock lock{mutex};
        binaryVersion = pBinaryVersion;
    }

    void TimeZoneManager::AddOperationEvent(const std::shared_ptr<kernel::type::KEvent> &event) {
        if (!event)
            return;

        std::scoped_lock lock{mutex};
        for (const auto &entry : operationEvents) {
            if (auto existing{entry.lock()}; existing == event)
                return;
        }
        operationEvents.emplace_back(event);
    }

    Result TimeZoneManager::ParseTimeZoneBinary(span<u8> binary, span<u8> ruleOut) {
        if (binary.empty() || ruleOut.empty())
            return result::InvalidArgument;

        auto ruleObj{tz_tzalloc(binary.data(), static_cast<long>(binary.size()))};
        if (!ruleObj)
            return result::RuleConversionFailed;

        std::memcpy(ruleOut.data(), ruleObj, ruleOut.size_bytes());
        tz_tzfree(ruleObj);
        return {};
    }

    ResultValue<FullCalendarTime> TimeZoneManager::ToCalendarTime(tz_timezone_t pRule, PosixTime posixTime) {
        if (!pRule)
            return result::InvalidArgument;

        struct tm tmp{};
        auto posixCalendarTime{tz_localtime_rz(pRule, &posixTime, &tmp)};
        if (!posixCalendarTime)
            return result::TimeZoneOutOfRange;

        FullCalendarTime out{
            .calendarTime{
                .year = static_cast<u16>(posixCalendarTime->tm_year + 1900),
                .month = static_cast<u8>(posixCalendarTime->tm_mon + 1),
                .day = static_cast<u8>(posixCalendarTime->tm_mday),
                .hour = static_cast<u8>(posixCalendarTime->tm_hour),
                .minute = static_cast<u8>(posixCalendarTime->tm_min),
                .second = static_cast<u8>(posixCalendarTime->tm_sec),
            },
            .additionalInfo{
                .dayOfWeek = static_cast<u32>(posixCalendarTime->tm_wday),
                .dayOfYear = static_cast<u32>(posixCalendarTime->tm_yday),
                .dst = static_cast<u32>(posixCalendarTime->tm_isdst > 0),
                .gmtOffset = static_cast<i32>(posixCalendarTime->tm_gmtoff),
            },
        };

        if (posixCalendarTime->tm_zone) {
            const std::string_view timeZoneName(posixCalendarTime->tm_zone);
            const auto length{std::min(timeZoneName.size(), out.additionalInfo.timeZoneName.size() - 1)};
            std::memcpy(out.additionalInfo.timeZoneName.data(), timeZoneName.data(), length);
        }
        return out;
    }

    ResultValue<FullCalendarTime> TimeZoneManager::ToCalendarTimeWithMyRule(PosixTime posixTime) {
        std::scoped_lock lock{mutex};
        if (!initialized)
            return result::ClockUninitialized;
        return ToCalendarTime(rule, posixTime);
    }

    ResultValue<PosixTime> TimeZoneManager::ToPosixTime(tz_timezone_t pRule, CalendarTime calendarTime) {
        if (!pRule)
            return result::InvalidArgument;
        if (calendarTime.month < 1 || calendarTime.month > 12 || calendarTime.day < 1 || calendarTime.day > 31 ||
            calendarTime.hour > 23 || calendarTime.minute > 59 || calendarTime.second > 60)
            return result::TimeZoneOutOfRange;

        struct tm posixCalendarTime{
            .tm_sec = calendarTime.second,
            .tm_min = calendarTime.minute,
            .tm_hour = calendarTime.hour,
            .tm_mday = calendarTime.day,
            .tm_mon = calendarTime.month - 1,
            .tm_year = calendarTime.year - 1900,
            .tm_isdst = -1,
        };

        return static_cast<PosixTime>(tz_mktime_z(pRule, &posixCalendarTime));
    }

    ResultValue<PosixTime> TimeZoneManager::ToPosixTimeWithMyRule(CalendarTime calendarTime) {
        std::scoped_lock lock{mutex};
        if (!initialized)
            return result::ClockUninitialized;
        return ToPosixTime(rule, calendarTime);
    }
}
