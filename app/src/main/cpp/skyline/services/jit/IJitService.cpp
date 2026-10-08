// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only stub. Not suitable for gameplay or master.

#include <kernel/results.h>
#include "IJitService.h"

namespace skyline::service::jit {
    IJitService::IJitService(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    IJitEnvironment::IJitEnvironment(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    Result IJitService::CreateJitEnvironment(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.cmdArgSz < sizeof(u64) * 2) {
            LOGW("JIT_DIAG: CreateJitEnvironment malformed request (raw bytes=0x{:X})", request.cmdArgSz);
            return kernel::result::InvalidArgument;
        }

        const u64 executableSize{request.Pop<u64>()};
        const u64 readableSize{request.Pop<u64>()};
        LOGW("JIT_DIAG: CreateJitEnvironment exec_size=0x{:X}, ro_size=0x{:X}, copied_handles={}, moved_handles={}",
             executableSize, readableSize, request.copyHandles.size(), request.moveHandles.size());

        // The real service requires a Process handle and CodeMemory handles.
        // We deliberately do not touch/mirror these handles: CodeMemory is not
        // implemented on this diagnostic branch.
        // For IPC tracing only, expose an IJitEnvironment session. None of its
        // commands will claim successful code generation or plugin initialization.
        LOGW("JIT_DIAG: returning an IPC-only environment; no executable memory is available");
        manager.RegisterService(SRVREG(IJitEnvironment), session, response);
        return {};
    }

    Result IJitEnvironment::GenerateCode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GenerateCode called (args=0x{:X}, input_buffers={}, output_buffers={}); unsupported",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::Control(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: Control called (args=0x{:X}, input_buffers={}, output_buffers={}); unsupported",
             request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::LoadPlugin(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: LoadPlugin called (args=0x{:X}, copied_handles={}, moved_handles={}, input_buffers={}); unsupported",
             request.cmdArgSz, request.copyHandles.size(), request.moveHandles.size(), request.inputBuf.size());
        return kernel::result::NotImplemented;
    }

    Result IJitEnvironment::GetCodeAddress(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGW("JIT_DIAG: GetCodeAddress called; no CodeMemory mappings exist");
        return kernel::result::NotImplemented;
    }
}
