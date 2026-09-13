// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "shared_mem.h"

namespace skyline::input {
    class MouseManager {
      private:
        bool activated{};
        MouseSection &section;
        std::mutex mutex;
        MouseState state{};

      public:
        explicit MouseManager(HidSharedMemory *hid);

        void Activate();
        void SetState(i32 x, i32 y, i32 deltaX, i32 deltaY, i32 wheelX, i32 wheelY, u32 buttons);
        void UpdateSharedMemory();
    };
}
