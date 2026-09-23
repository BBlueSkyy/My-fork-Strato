// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "backing.h"

namespace skyline::vfs {
    /**
     * @brief The OsBacking class provides the backing abstractions for a physical linux file
     */
    class OsBacking : public Backing {
      private:
        int fd; //!< An FD to the backing
        bool closable; //!< Whether the FD can be closed when the backing is destroyed

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override;

        std::pair<size_t, std::error_code> WriteWithErrorImpl(span<u8> input, size_t offset) override;

        std::error_code ResizeWithErrorImpl(size_t size) override;

        std::error_code FlushImpl() override;

      public:
        /**
         * @param fd The file descriptor of the backing
         */
        OsBacking(int fd, bool closable = false, Mode = {true, false, false});

        ~OsBacking();
    };
}
