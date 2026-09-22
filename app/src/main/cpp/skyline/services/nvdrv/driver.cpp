// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "driver.h"
#include "devices/nvmap.h"
#include "devices/nvhost/ctrl.h"
#include "devices/nvhost/ctrl_gpu.h"
#include "devices/nvhost/gpu_channel.h"
#include "devices/nvhost/as_gpu.h"
#include "devices/nvhost/host1x_channel.h"
#include <xv2_trace.h>

namespace skyline::service::nvdrv {
    Driver::Driver(const DeviceState &state) : state(state), core(state) {}

    NvResult Driver::OpenDevice(std::string_view path, FileDescriptor fd, const SessionContext &ctx) {
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
            auto &device{devices.at(fd)};
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, device->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            u32 traceSeq{256};
            if (diagnostics::xv2::PostCloseActive()) {
                traceSeq = diagnostics::xv2::NextNvdrvSequence();
                if (traceSeq < 256)
                    LOGI("XV2-NVDRV ioctl-enter epoch={} seq={} kind=1 fd={} device={} raw=0x{:X} magic=0x{:X} function=0x{:X} size=0x{:X} in={} out={}",
                         diagnostics::xv2::Epoch(), traceSeq, fd, device->GetName(), cmd.raw,
                         static_cast<u8>(cmd.magic), cmd.function, static_cast<u16>(cmd.size),
                         static_cast<bool>(cmd.in), static_cast<bool>(cmd.out));
            }
            auto result{ConvertResult(LogIoctlResult(device->Ioctl(cmd, buffer), cmd.raw))};
            if (traceSeq < 256)
                LOGI("XV2-NVDRV ioctl-return epoch={} seq={} kind=1 fd={} raw=0x{:X} result=0x{:X}",
                     diagnostics::xv2::Epoch(), traceSeq, fd, cmd.raw, static_cast<i32>(result));
            return result;
        } catch (const std::out_of_range &) {
            throw exception("Ioctl was called with invalid fd: {}", fd);
        }
    }

    NvResult Driver::Ioctl2(FileDescriptor fd, IoctlDescriptor cmd, span<u8> buffer, span<u8> inlineBuffer) {
        try {
            std::shared_lock lock(deviceMutex);
            auto &device{devices.at(fd)};
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, device->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            u32 traceSeq{256};
            if (diagnostics::xv2::PostCloseActive()) {
                traceSeq = diagnostics::xv2::NextNvdrvSequence();
                if (traceSeq < 256)
                    LOGI("XV2-NVDRV ioctl-enter epoch={} seq={} kind=2 fd={} device={} raw=0x{:X} magic=0x{:X} function=0x{:X} size=0x{:X} in={} out={} inline-size={}",
                         diagnostics::xv2::Epoch(), traceSeq, fd, device->GetName(), cmd.raw,
                         static_cast<u8>(cmd.magic), cmd.function, static_cast<u16>(cmd.size),
                         static_cast<bool>(cmd.in), static_cast<bool>(cmd.out), inlineBuffer.size());
            }
            auto result{ConvertResult(LogIoctlResult(device->Ioctl2(cmd, buffer, inlineBuffer), cmd.raw))};
            if (traceSeq < 256)
                LOGI("XV2-NVDRV ioctl-return epoch={} seq={} kind=2 fd={} raw=0x{:X} result=0x{:X}",
                     diagnostics::xv2::Epoch(), traceSeq, fd, cmd.raw, static_cast<i32>(result));
            return result;
        } catch (const std::out_of_range &) {
            throw exception("Ioctl2 was called with invalid fd: {}", fd);
        }
    }

    NvResult Driver::Ioctl3(FileDescriptor fd, IoctlDescriptor cmd, span<u8> buffer, span<u8> inlineBuffer) {
        try {
            std::shared_lock lock(deviceMutex);
            auto &device{devices.at(fd)};
            LOGD("fd: {}, cmd: 0x{:X}, device: {}", fd, cmd.raw, device->GetName());
            TRACE_EVENT("service", "Ioctl", "fd", fd, "cmd", cmd.raw);
            u32 traceSeq{256};
            if (diagnostics::xv2::PostCloseActive()) {
                traceSeq = diagnostics::xv2::NextNvdrvSequence();
                if (traceSeq < 256)
                    LOGI("XV2-NVDRV ioctl-enter epoch={} seq={} kind=3 fd={} device={} raw=0x{:X} magic=0x{:X} function=0x{:X} size=0x{:X} in={} out={} inline-size={}",
                         diagnostics::xv2::Epoch(), traceSeq, fd, device->GetName(), cmd.raw,
                         static_cast<u8>(cmd.magic), cmd.function, static_cast<u16>(cmd.size),
                         static_cast<bool>(cmd.in), static_cast<bool>(cmd.out), inlineBuffer.size());
            }
            auto result{ConvertResult(LogIoctlResult(device->Ioctl3(cmd, buffer, inlineBuffer), cmd.raw))};
            if (traceSeq < 256)
                LOGI("XV2-NVDRV ioctl-return epoch={} seq={} kind=3 fd={} raw=0x{:X} result=0x{:X}",
                     diagnostics::xv2::Epoch(), traceSeq, fd, cmd.raw, static_cast<i32>(result));
            return result;
        } catch (const std::out_of_range &) {
            throw exception("Ioctl3 was called with invalid fd: {}", fd);
        }
    }

    void Driver::CloseDevice(FileDescriptor fd) {
        try {
            std::unique_lock lock(deviceMutex);
            if (diagnostics::xv2::PostCloseActive()) {
                auto seq{diagnostics::xv2::NextNvdrvSequence()};
                if (seq < 256)
                    LOGI("XV2-NVDRV close epoch={} seq={} fd={} device={}",
                         diagnostics::xv2::Epoch(), seq, fd,
                         devices.contains(fd) ? devices.at(fd)->GetName() : "<invalid>");
            }
            devices.erase(fd);
        } catch (const std::out_of_range &) {
            LOGW("Trying to close invalid fd: {}", fd);
        }
    }

    std::shared_ptr<kernel::type::KEvent> Driver::QueryEvent(FileDescriptor fd, u32 eventId) {
        LOGD("fd: {}, eventId: 0x{:X}, device: {}", fd, eventId, devices.at(fd)->GetName());

        try {
            std::shared_lock lock(deviceMutex);
            auto &device{devices.at(fd)};
            if (diagnostics::xv2::PostCloseActive()) {
                auto seq{diagnostics::xv2::NextNvdrvSequence()};
                if (seq < 256)
                    LOGI("XV2-NVDRV query-event epoch={} seq={} fd={} device={} event-id=0x{:X}",
                         diagnostics::xv2::Epoch(), seq, fd, device->GetName(), eventId);
            }
            return device->QueryEvent(eventId);
        } catch (const std::exception &) {
            throw exception("QueryEvent was called with invalid fd: {}", fd);
        }
    }
}
