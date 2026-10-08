// SPDX-License-Identifier: MPL-2.0
// JIT sysmodule HLE with real plugin execution and shared CodeMemory pages.

#pragma once

#include <services/serviceman.h>
#include <kernel/types/KCodeMemory.h>
#include "plugin_context.h"

namespace skyline::kernel::type { class KTransferMemory; }

namespace skyline::service::jit {
    /** Owns the compiler plugin and its CodeMemory mappings for one IPC session. */
    class IJitEnvironment : public BaseService {
      private:
        // Weak process reference avoids a process/session/CodeMemory ownership cycle.
        std::weak_ptr<kernel::type::KProcess> process;
        std::shared_ptr<kernel::type::KCodeMemory> executableMemory;
        std::shared_ptr<kernel::type::KCodeMemory> readableMemory;

        struct CodeRange { u64 offset, size; };
        struct Configuration {
            CodeRange userRx, userRo, transfer, sysRx, sysRo;
        } configuration{};
        static_assert(sizeof(Configuration) == 0x50);
        PluginImage image;
        std::unique_ptr<PluginContext> context;
        std::shared_ptr<kernel::type::KTransferMemory> transferMemory;
        std::mutex mutex;
        bool prepared{};
        void Synchronize();

      public:
        IJitEnvironment(const DeviceState &state, ServiceManager &manager,
                        std::shared_ptr<kernel::type::KProcess> process,
                        std::shared_ptr<kernel::type::KCodeMemory> executableMemory,
                        std::shared_ptr<kernel::type::KCodeMemory> readableMemory);

        ~IJitEnvironment();
        Result Initialize(u64 executableSize, u64 readableSize);

        Result GenerateCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Control(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LoadPlugin(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCodeAddress(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, IJitEnvironment, GenerateCode),
            SFUNC(1, IJitEnvironment, Control),
            SFUNC(1000, IJitEnvironment, LoadPlugin),
            SFUNC(1001, IJitEnvironment, GetCodeAddress)
        )
    };

    /**
     * @brief nn::jitsrv::IJitService (jit:u).
     */
    class IJitService : public BaseService {
      public:
        IJitService(const DeviceState &state, ServiceManager &manager);

        Result CreateJitEnvironment(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, IJitService, CreateJitEnvironment)
        )
    };
}
