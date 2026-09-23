// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "os_backing.h"

namespace skyline::vfs {
    OsBacking::OsBacking(int fd, bool closable, Mode mode) : Backing(mode), fd(fd), closable(closable) {
        struct stat fileInfo;
        if (fstat(fd, &fileInfo))
            throw exception("Failed to stat fd: {}", strerror(errno));

        size = static_cast<size_t>(fileInfo.st_size);
    }

    OsBacking::~OsBacking() {
        if (closable)
            close(fd);
    }

    size_t OsBacking::ReadImpl(span<u8> output, size_t offset) {
        size_t bytesRead{};
        while (bytesRead < output.size()) {
            auto ret{pread64(fd, output.data() + bytesRead, output.size() - bytesRead, static_cast<off64_t>(offset + bytesRead))};
            if (ret < 0) {
                if (errno == EFAULT) {
                    // If EFAULT is returned then we're reading into a trapped region so create a temporary buffer and read into that instead
                    // This is required since pread doesn't trigger signal handlers itself
                    std::vector<u8> buffer(output.size() - bytesRead);
                    ret = pread64(fd, buffer.data(), buffer.size(), static_cast<off64_t>(offset + bytesRead));
                    if (ret >= 0) {
                        output.subspan(bytesRead).copy_from(buffer);
                        bytesRead += static_cast<size_t>(ret);
                        continue;
                    }
                }

                throw exception("Failed to read from fd: {}", strerror(errno));
            } else if (ret == 0) {
                LOGE("OsBacking: short read - offset=0x{:X} requested=0x{:X} got=0x{:X} fileSize=0x{:X}", offset, output.size(), bytesRead, size);
                return bytesRead;
            } else {
                bytesRead += static_cast<size_t>(ret);
            }
        }
        return output.size();
    }

    std::pair<size_t, std::error_code> OsBacking::WriteWithErrorImpl(span<u8> input, size_t offset) {
        size_t bytesWritten{};
        while (bytesWritten < input.size()) {
            const auto ret{pwrite64(fd, input.data() + bytesWritten, input.size() - bytesWritten, static_cast<off64_t>(offset + bytesWritten))};
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                return {bytesWritten, {errno, std::generic_category()}};
            }
            if (ret == 0)
                return {bytesWritten, std::make_error_code(std::errc::io_error)};
            bytesWritten += static_cast<size_t>(ret);
        }
        return {bytesWritten, {}};
    }

    std::error_code OsBacking::ResizeWithErrorImpl(size_t pSize) {
        if (pSize > static_cast<size_t>(std::numeric_limits<off_t>::max()))
            return std::make_error_code(std::errc::value_too_large);
        if (ftruncate(fd, static_cast<off_t>(pSize)) < 0)
            return {errno, std::generic_category()};

        size = pSize;
        return {};
    }

    std::error_code OsBacking::FlushImpl() {
        if (fsync(fd) < 0)
            return {errno, std::generic_category()};
        return {};
    }
}
