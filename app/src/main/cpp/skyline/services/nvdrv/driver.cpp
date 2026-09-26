// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "driver.h"
#include "devices/nvmap.h"
#include "devices/nvhost/ctrl.h"
#include "devices/nvhost/ctrl_gpu.h"
#include "devices/nvhost/gpu_channel.h"
#include "devices/nvhost/as_gpu.h"
#include "devices/nvhost/host1x_channel.h"

namespace skyline::service::nvdrv {
    Driver::Driver(const DeviceState &state) : state(state), core(state) {}

    NvResult Driver::OpenDevice(std::string_view path, FileDescriptor fd, const SessionContext &ctx) {
        LOGI("GRID-LIFE Driver::OpenDevice begin fd={} path={}", fd, path);
        LOGD("Opening NvDrv device ({}): {}", fd, path);
        auto pathHash{util::Hash(path)};

        #define DEVICE_SWITCH(cases) \
            switch (pathHash) {      \
                cases;               \
                default:             \
                    break;           \
            }

        #define DEVICE_CASE(path, object, ...)                                                                     \
            case util::Hash(path):                                                                                 \
                {                                                                                                  \
                    std::unique_lock lock(deviceMutex);                                                            \
                    devices.emplace(fd, std::make_unique<device::object>(state, *this, core, ctx, ##__VA_ARGS__)); \
                    LOGI("GRID-LIFE Driver::OpenDevice success fd={} path={} object={}",                         \
                         fd, path, static_cast<const void *>(devices.at(fd).get()));                               \
                    return NvResult::Success;                                                                      \
                }

        DEVICE_SWITCH(
            DEVICE_CASE("/dev/nvmap", NvMap)
            DEVICE_CASE("/dev/nvhost-ctrl", nvhost::Ctrl)
        )

        if (ctx.perms.AccessGpu) {
            DEVICE_SWITCH(
                DEVICE_CASE("/dev/nvhost-as-gpu", nvhost::AsGpu)
                DEVICE_CASE("/dev/nvhost-ctrl-gpu", nvhost::CtrlGpu)
                DEVICE_CASE("/dev/nvhost-gpu", nvhost::GpuChannel)
            )
        }

        if (ctx.perms.AccessJpeg)
            DEVICE_SWITCH(DEVICE_CASE("/dev/nvhost-nvjpg", nvhost::Host1xChannel, core::ChannelType::NvJpg))

        if (ctx.perms.AccessVic)
            DEVICE_SWITCH(DEVICE_CASE("/dev/nvhost-vic", nvhost::Host1xChannel, core::ChannelType::VIC))

        if (ctx.perms.AccessVideoDecoder)
            DEVICE_SWITCH(DEVICE_CASE("/dev/nvhost-nvdec", nvhost::Host1xChannel, core::ChannelType::NvDec))

        #undef DEVICE_CASE
        #undef DEVICE_SWITCH

        // Device doesn't exist/no permissions
        return NvResult::FileOperationFailed;
    }

    static PosixResult LogIoctlResult(PosixResult result, u32 ioctl) {
        switch (result) {
            case PosixResult::Success:
            case PosixResult::TryAgain:
            case PosixResult::Busy:
            case PosixResult::TimedOut:
                return result;
            case PosixResult::NotPermitted:
            case PosixResult::InvalidArgument:
            case PosixResult::InappropriateIoctlForDevice:
            case PosixResult::NotSupported:
            default:
                constexpr u32 GetConfigIoctl{0xC183001B};
                // GetConfig is the only ioctl that's expected to fail with one of these errors in normal use so ignore it
                if (ioctl != GetConfigIoctl)
                    LOGW("IOCTL {} failed: 0x{:X}", ioctl, static_cast<i32>(result));
                return result;
        }
    }

    static NvResult ConvertResult(PosixResult result) {
        switch (result) {
            case PosixResult::Success:
                return NvResult::Success;
            case PosixResult::NotPermitted:
                return NvResult::AccessDenied;
            case PosixResult::TryAgain:
                return NvResult::Timeout;
            case PosixResult::Busy:
                return NvResult::Busy;
            case PosixResult::InvalidArgument:
                return NvResult::BadValue;
            case PosixResult::InappropriateIoctlForDevice:
                return NvResult::IoctlFailed;
            case PosixResult::NotSupported:
                return NvResult::NotSupported;
            case PosixResult::TimedOut:
                return NvResult::Timeout;
            default:
                throw exception("Unhandled POSIX result: {}!", static_cast<i32>(result));
        }
    }

    NvResult Driver::Ioctl(FileDescriptor fd, IoctlDescriptor cmd, span<u8> buffer) {
        try {
            std::shared_lock lock(deviceMutex);
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, devices.at(fd)->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            return ConvertResult(LogIoctlResult(devices.at(fd)->Ioctl(cmd, buffer), cmd.raw));
        } catch (const std::out_of_range &) {
            throw exception("Ioctl was called with invalid fd: {}", fd);
        }
    }

    NvResult Driver::Ioctl2(FileDescriptor fd, IoctlDescriptor cmd, span<u8> buffer, span<u8> inlineBuffer) {
        try {
            std::shared_lock lock(deviceMutex);
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, devices.at(fd)->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            return ConvertResult(LogIoctlResult(devices.at(fd)->Ioctl2(cmd, buffer, inlineBuffer), cmd.raw));
        } catch (const std::out_of_range &) {
            throw exception("Ioctl2 was called with invalid fd: {}", fd);
        }
    }

    NvResult Driver::Ioctl3(FileDescriptor fd, IoctlDescriptor cmd, span<u8> buffer, span<u8> inlineBuffer) {
        try {
            std::shared_lock lock(deviceMutex);
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, devices.at(fd)->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            return ConvertResult(LogIoctlResult(devices.at(fd)->Ioctl3(cmd, buffer, inlineBuffer), cmd.raw));
        } catch (const std::out_of_range &) {
            throw exception("Ioctl3 was called with invalid fd: {}", fd);
        }
    }

    void Driver::CloseDevice(FileDescriptor fd) {
        try {
            std::unique_lock lock(deviceMutex);
            auto it{devices.find(fd)};
            const void *object{it != devices.end() ? static_cast<const void *>(it->second.get()) : nullptr};
            LOGI("GRID-LIFE Driver::CloseDevice begin fd={} object={} devices={}", fd, object, devices.size());
            devices.erase(fd);
            LOGI("GRID-LIFE Driver::CloseDevice end fd={} object={} devices={}", fd, object, devices.size());
        } catch (const std::out_of_range &) {
            LOGW("Trying to close invalid fd: {}", fd);
        }
    }

    std::shared_ptr<kernel::type::KEvent> Driver::QueryEvent(FileDescriptor fd, u32 eventId) {
        LOGD("fd: {}, eventId: 0x{:X}, device: {}", fd, eventId, devices.at(fd)->GetName());

        try {
            std::shared_lock lock(deviceMutex);
            return devices.at(fd)->QueryEvent(eventId);
        } catch (const std::exception &) {
            throw exception("QueryEvent was called with invalid fd: {}", fd);
        }
    }
}
