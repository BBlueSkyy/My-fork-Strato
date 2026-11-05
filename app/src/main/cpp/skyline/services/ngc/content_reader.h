// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <functional>
#include <common.h>

namespace skyline::vfs { class FileSystem; }

namespace skyline::service::ngc {
    // All files in one reader belong to one snapshot of system title 0100000000000823.
    // Never combine dictionaries from different firmware archives.
    class ContentReader {
      public:
        using Mount = std::function<std::shared_ptr<vfs::FileSystem>()>;
        static constexpr size_t MaxFileSize{64 * 1024 * 1024};
        explicit ContentReader(const DeviceState &state);
        explicit ContentReader(Mount mount);
        bool ReadFile(std::string_view name, std::vector<u8> &output);
        bool Reload();
        u32 Version();
      private:
        Mount mount;
        std::shared_ptr<vfs::FileSystem> filesystem;
        bool attempted{};
        bool EnsureMounted();
    };
}
