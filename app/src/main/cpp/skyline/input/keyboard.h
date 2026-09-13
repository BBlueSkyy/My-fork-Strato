// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "shared_mem.h"

namespace skyline::input {
    class KeyboardManager {
      private:
        bool activated{};
        KeyboardSection &section;
        std::mutex mutex;
        KeyboardState state{};

      public:
        explicit KeyboardManager(HidSharedMemory *hid);

        void Activate();
        void SetKeyState(u32 usage, bool pressed, u32 modifiers);
        void UpdateSharedMemory();
    };
}
