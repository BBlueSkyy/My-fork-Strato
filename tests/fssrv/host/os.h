// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vfs/filesystem.h>

namespace skyline::kernel {
    class OS {
      public:
        std::string publicAppFilesPath;
        std::string privateAppFilesPath;
        std::shared_ptr<vfs::FileSystem> assetFileSystem;
    };
}
