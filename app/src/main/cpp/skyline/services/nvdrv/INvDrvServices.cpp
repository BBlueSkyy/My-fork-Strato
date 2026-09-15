// SPDX-License-Identifier: MIT OR MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include <chrono>
#include <numeric>
#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include "INvDrvServices.h"
#include "driver.h"
#include "devices/nvdevice.h"

#define NVRESULT(x) [&response](NvResult err) {         \
        if (err != NvResult::Success)                   \
            LOGD("IOCTL Failed: 0x{:X}", err); \
        response.Push<NvResult>(err);                   \
        return Result{};                                \
    } (x)

namespace skyline::service::nvdrv {
    namespace {
        using NvTraceClock = std::chrono::steady_clock;

        std::atomic<u64> nvTraceSequence{1};

        u64 NextNvTraceSequence() {
            return nvTraceSequence.fetch_add(1, std::memory_order_relaxed);
        }

        u64 NvTraceTimestampUs() {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(NvTraceClock::now().time_since_epoch()).count());
        }

        u64 NvTraceDurationUs(NvTraceClock::time_point start) {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(NvTraceClock::now() - start).count());
        }

        u64 HashBuffer(span<u8> buffer) {
            constexpr u64 FnvOffsetBasis{14695981039346656037ULL};
            constexpr u64 FnvPrime{1099511628211ULL};

            u64 hash{FnvOffsetBasis};
            for (size_t i{}; i < buffer.size(); ++i) {
                hash ^= buffer[i];
                hash *= FnvPrime;
            }
            return hash;
        }
    }

    INvDrvServices::INvDrvServices(const DeviceState &state, ServiceManager &manager, Driver &driver, const SessionPermissions &perms)
        : BaseService(state, manager),
          driver(driver),
          ctx(SessionContext{.perms = perms}) {}

    Result INvDrvServices::Open(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        constexpr FileDescriptor SessionFdLimit{std::numeric_limits<u64>::digits * 2}; //!< Nvdrv uses two 64 bit variables to store a bitset

        auto path{request.inputBuf.at(0).as_string(true)};
        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};

        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=Open timestamp_us={} path='{}'", seq, NvTraceTimestampUs(), path);

        if (path.empty() || nextFdIndex == SessionFdLimit) {
            response.Push<FileDescriptor>(InvalidFileDescriptor);
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=Open timestamp_us={} duration_us={} path='{}' fd={} device='<none>' result=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), path, InvalidFileDescriptor,
                     static_cast<i32>(NvResult::FileOperationFailed));
            return NVRESULT(NvResult::FileOperationFailed);
        }

        const FileDescriptor fd{nextFdIndex};
        if (auto err{driver.OpenDevice(path, fd, ctx)}; err != NvResult::Success) {
            response.Push<FileDescriptor>(InvalidFileDescriptor);
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=Open timestamp_us={} duration_us={} path='{}' fd={} device='<none>' result=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), path, InvalidFileDescriptor, static_cast<i32>(err));
            return NVRESULT(err);
        }

        response.Push(nextFdIndex++);
        if (trace)
            LOGI("[NVTRACE][EXIT] seq={} op=Open timestamp_us={} duration_us={} path='{}' fd={} device='{}' result=0x{:X}",
                 seq, NvTraceTimestampUs(), NvTraceDurationUs(start), path, fd, driver.GetDeviceName(fd),
                 static_cast<i32>(NvResult::Success));
        return NVRESULT(NvResult::Success);
    }

    static NvResultValue<span<u8>> GetMainIoctlBuffer(IoctlDescriptor ioctl, span<u8> inBuf, span<u8> outBuf) {
        if (ioctl.in && inBuf.size() < ioctl.size)
            return NvResult::InvalidSize;

        if (ioctl.out && outBuf.size() < ioctl.size)
            return NvResult::InvalidSize;

        if (ioctl.in && ioctl.out) {
            if (outBuf.size() < inBuf.size())
                return NvResult::InvalidSize;

            // Copy in buf to out buf for inout ioctls to avoid needing to pass around two buffers everywhere
            if (outBuf.data() != inBuf.data())
                outBuf.copy_from(inBuf, ioctl.size);
        }

        return ioctl.out ? outBuf : inBuf;
    }

    Result INvDrvServices::Ioctl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto fd{request.Pop<FileDescriptor>()};
        auto ioctl{request.Pop<IoctlDescriptor>()};

        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};
        const auto device{trace ? driver.GetDeviceName(fd) : std::string{}};
        const size_t inputSize{!request.inputBuf.empty() ? request.inputBuf.at(0).size() : 0};
        const size_t outputSize{!request.outputBuf.empty() ? request.outputBuf.at(0).size() : 0};
        const u32 magic{static_cast<u32>(static_cast<u8>(ioctl.magic))};
        const u32 function{ioctl.function};
        const u32 declaredSize{ioctl.size};
        const bool inFlag{ioctl.in};
        const bool outFlag{ioctl.out};

        auto buf{GetMainIoctlBuffer(ioctl,
                                    !request.inputBuf.empty() ? request.inputBuf.at(0) : span<u8>{},
                                    !request.outputBuf.empty() ? request.outputBuf.at(0) : span<u8>{})};

        const u64 hashBefore{buf ? HashBuffer(*buf) : 0};
        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=Ioctl timestamp_us={} fd={} device='{}' cmd=0x{:X} magic=0x{:X} function=0x{:X} declaredSize=0x{:X} in={} out={} inputSize=0x{:X} outputSize=0x{:X} bufferHashBefore=0x{:X}",
                 seq, NvTraceTimestampUs(), fd, device, ioctl.raw, magic, function, declaredSize,
                 inFlag, outFlag, inputSize, outputSize, hashBefore);

        if (!buf) {
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=Ioctl timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x0",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                     static_cast<i32>(NvResult::InvalidSize));
            return NVRESULT(buf);
        }

        const NvResult result{driver.Ioctl(fd, ioctl, *buf)};
        const u64 hashAfter{HashBuffer(*buf)};
        if (trace)
            LOGI("[NVTRACE][EXIT] seq={} op=Ioctl timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x{:X}",
                 seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                 static_cast<i32>(result), hashAfter);
        return NVRESULT(result);
    }

    Result INvDrvServices::Close(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto fd{request.Pop<FileDescriptor>()};
        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};
        const auto device{trace ? driver.GetDeviceName(fd) : std::string{}};

        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=Close timestamp_us={} fd={} device='{}'", seq, NvTraceTimestampUs(), fd, device);

        LOGD("Closing NVDRV device ({})", fd);
        driver.CloseDevice(fd);

        if (trace)
            LOGI("[NVTRACE][EXIT] seq={} op=Close timestamp_us={} duration_us={} fd={} device='{}' result=0x{:X}",
                 seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, static_cast<i32>(NvResult::Success));
        return NVRESULT(NvResult::Success);
    }

    Result INvDrvServices::Initialize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return NVRESULT(NvResult::Success);
    }

    Result INvDrvServices::QueryEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto fd{request.Pop<FileDescriptor>()};
        auto eventId{request.Pop<u32>()};
        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};
        const auto device{trace ? driver.GetDeviceName(fd) : std::string{}};

        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=QueryEvent timestamp_us={} fd={} device='{}' eventId=0x{:X}",
                 seq, NvTraceTimestampUs(), fd, device, eventId);

        auto event{driver.QueryEvent(fd, eventId)};

        if (event != nullptr) {
            auto handle{state.process->InsertItem<type::KEvent>(event)};

            LOGD("FD: {}, Event ID: {}, Handle: 0x{:X}", fd, eventId, handle);
            response.copyHandles.push_back(handle);

            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=QueryEvent timestamp_us={} duration_us={} fd={} device='{}' eventId=0x{:X} eventFound=true guestHandle=0x{:X} result=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, eventId, handle,
                     static_cast<i32>(NvResult::Success));
            return NVRESULT(NvResult::Success);
        } else {
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=QueryEvent timestamp_us={} duration_us={} fd={} device='{}' eventId=0x{:X} eventFound=false guestHandle=0x0 result=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, eventId,
                     static_cast<i32>(NvResult::BadValue));
            return NVRESULT(NvResult::BadValue);
        }
    }

    Result INvDrvServices::Ioctl2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto fd{request.Pop<FileDescriptor>()};
        auto ioctl{request.Pop<IoctlDescriptor>()};

        // Inline buffer is optional
        auto inlineBuf{request.inputBuf.size() > 1 ? request.inputBuf.at(1) : span<u8>{}};

        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};
        const auto device{trace ? driver.GetDeviceName(fd) : std::string{}};
        const size_t inputSize{!request.inputBuf.empty() ? request.inputBuf.at(0).size() : 0};
        const size_t outputSize{!request.outputBuf.empty() ? request.outputBuf.at(0).size() : 0};
        const u32 magic{static_cast<u32>(static_cast<u8>(ioctl.magic))};
        const u32 function{ioctl.function};
        const u32 declaredSize{ioctl.size};
        const bool inFlag{ioctl.in};
        const bool outFlag{ioctl.out};
        const u64 inlineHash{HashBuffer(inlineBuf)};

        auto buf{GetMainIoctlBuffer(ioctl,
                                    !request.inputBuf.empty() ? request.inputBuf.at(0) : span<u8>{},
                                    !request.outputBuf.empty() ? request.outputBuf.at(0) : span<u8>{})};

        const u64 hashBefore{buf ? HashBuffer(*buf) : 0};
        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=Ioctl2 timestamp_us={} fd={} device='{}' cmd=0x{:X} magic=0x{:X} function=0x{:X} declaredSize=0x{:X} in={} out={} inputSize=0x{:X} outputSize=0x{:X} inlineInputSize=0x{:X} bufferHashBefore=0x{:X} inlineHash=0x{:X}",
                 seq, NvTraceTimestampUs(), fd, device, ioctl.raw, magic, function, declaredSize,
                 inFlag, outFlag, inputSize, outputSize, inlineBuf.size(), hashBefore, inlineHash);

        if (!buf) {
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=Ioctl2 timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x0 inlineHash=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                     static_cast<i32>(NvResult::InvalidSize), inlineHash);
            return NVRESULT(buf);
        }

        const NvResult result{driver.Ioctl2(fd, ioctl, *buf, inlineBuf)};
        const u64 hashAfter{HashBuffer(*buf)};
        if (trace)
            LOGI("[NVTRACE][EXIT] seq={} op=Ioctl2 timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x{:X} inlineHash=0x{:X}",
                 seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                 static_cast<i32>(result), hashAfter, inlineHash);
        return NVRESULT(result);
    }

    Result INvDrvServices::Ioctl3(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto fd{request.Pop<FileDescriptor>()};
        auto ioctl{request.Pop<IoctlDescriptor>()};

        // Inline buffer is optional
        auto inlineBuf{request.outputBuf.size() > 1 ? request.outputBuf.at(1) : span<u8>{}};

        const bool trace{*state.settings->autoStub};
        const u64 seq{trace ? NextNvTraceSequence() : 0};
        const auto start{NvTraceClock::now()};
        const auto device{trace ? driver.GetDeviceName(fd) : std::string{}};
        const size_t inputSize{!request.inputBuf.empty() ? request.inputBuf.at(0).size() : 0};
        const size_t outputSize{!request.outputBuf.empty() ? request.outputBuf.at(0).size() : 0};
        const u32 magic{static_cast<u32>(static_cast<u8>(ioctl.magic))};
        const u32 function{ioctl.function};
        const u32 declaredSize{ioctl.size};
        const bool inFlag{ioctl.in};
        const bool outFlag{ioctl.out};
        const u64 inlineHashBefore{HashBuffer(inlineBuf)};

        auto buf{GetMainIoctlBuffer(ioctl,
                                    !request.inputBuf.empty() ? request.inputBuf.at(0) : span<u8>{},
                                    !request.outputBuf.empty() ? request.outputBuf.at(0) : span<u8>{})};

        const u64 hashBefore{buf ? HashBuffer(*buf) : 0};
        if (trace)
            LOGI("[NVTRACE][ENTER] seq={} op=Ioctl3 timestamp_us={} fd={} device='{}' cmd=0x{:X} magic=0x{:X} function=0x{:X} declaredSize=0x{:X} in={} out={} inputSize=0x{:X} outputSize=0x{:X} inlineOutputSize=0x{:X} bufferHashBefore=0x{:X} inlineHashBefore=0x{:X}",
                 seq, NvTraceTimestampUs(), fd, device, ioctl.raw, magic, function, declaredSize,
                 inFlag, outFlag, inputSize, outputSize, inlineBuf.size(), hashBefore, inlineHashBefore);

        if (!buf) {
            if (trace)
                LOGI("[NVTRACE][EXIT] seq={} op=Ioctl3 timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x0 inlineHashAfter=0x{:X}",
                     seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                     static_cast<i32>(NvResult::InvalidSize), HashBuffer(inlineBuf));
            return NVRESULT(buf);
        }

        const NvResult result{driver.Ioctl3(fd, ioctl, *buf, inlineBuf)};
        const u64 hashAfter{HashBuffer(*buf)};
        const u64 inlineHashAfter{HashBuffer(inlineBuf)};
        if (trace)
            LOGI("[NVTRACE][EXIT] seq={} op=Ioctl3 timestamp_us={} duration_us={} fd={} device='{}' cmd=0x{:X} result=0x{:X} bufferHashAfter=0x{:X} inlineHashAfter=0x{:X}",
                 seq, NvTraceTimestampUs(), NvTraceDurationUs(start), fd, device, ioctl.raw,
                 static_cast<i32>(result), hashAfter, inlineHashAfter);
        return NVRESULT(result);
    }

    Result INvDrvServices::GetStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        struct Status {
            u32 freeSize;
            u32 allocatableSize;
            u32 minimumFreeSize;
            u32 minimumAllocatableSize;
            u32 reserved;
        };

        // Return empty values since we don't use the transfer memory for allocations
        response.Push<Status>({});
        return NVRESULT(NvResult::Success);
    }

    Result INvDrvServices::SetAruid(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return NVRESULT(NvResult::Success);
    }

    Result INvDrvServices::DumpStatus(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result INvDrvServices::SetGraphicsFirmwareMemoryMarginEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }
}
