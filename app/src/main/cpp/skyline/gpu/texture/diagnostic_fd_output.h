// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cerrno>
#include <streambuf>
#include <unistd.h>

namespace skyline::gpu::texture::diagnostic {
    // Borrow the Android document provider descriptor. Do not reopen a procfs
    // path, seek, truncate or close it: the provider may supply a write-only pipe.
    class FdOutputBuffer : public std::streambuf {
        int fd;
      protected:
        std::streamsize xsputn(const char *data, std::streamsize size) override {
            std::streamsize done{};
            while (done < size) {
                const auto written{::write(fd, data + done, static_cast<size_t>(size - done))};
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) break;
                done += written;
            }
            return done;
        }
        int_type overflow(int_type value) override {
            if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
            const char byte{traits_type::to_char_type(value)};
            return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
        }
      public:
        explicit FdOutputBuffer(int descriptor) : fd{descriptor} {}
    };
}
