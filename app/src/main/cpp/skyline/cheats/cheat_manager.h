// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <atomic>
#include <thread>
#include "cheat_vm.h"

namespace skyline {
    namespace kernel::type { class KProcess; class KThread; }

    namespace cheats {
        struct MainNsoInfo {
            std::array<u8, 0x20> buildId{};
            u64 base{};
            u64 size{};
        };

        void RecordMainNso(const kernel::type::KProcess *process, const std::array<u64, 4> &buildId, u64 base, u64 size);
        void ClearMainNso(const kernel::type::KProcess *process);

        class CheatManager final : private CheatVm::Callbacks {
          public:
            CheatManager(const DeviceState &state, std::shared_ptr<kernel::type::KProcess> process);
            ~CheatManager();

            bool Active() const { return vm.ProgramSize() != 0; }

          private:
            const DeviceState &state;
            std::shared_ptr<kernel::type::KProcess> process;
            CheatProcessMetadata metadata{};
            std::vector<CheatEntry> entries;
            CheatVm vm;
            std::thread worker;
            std::atomic_bool stopRequested{};
            std::vector<std::shared_ptr<kernel::type::KThread>> pausedThreads;
            bool processPaused{};

            void Run();
            std::vector<CheatEntry> LoadCheats();

            bool ReadMemory(u64 address, void *data, size_t size) override;
            bool WriteMemory(u64 address, const void *data, size_t size) override;
            u64 KeysDown() override;
            void PauseProcess() override;
            void ResumeProcess() override;
            void DebugLog(u8 id, u64 value) override;
        };
    }
}
