// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <iostream>
#include <services/fssrv/types.h>
#include <services/fssrv/validation.h>

using namespace skyline;
using namespace skyline::service::fssrv;

namespace {
    void Check(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    void TestAbiLayouts() {
        static_assert(sizeof(SaveDataAttribute) == 0x40);
        static_assert(sizeof(SaveDataInfo) == 0x60);
        static_assert(sizeof(FileTimeStampRaw) == 0x20);
        static_assert(sizeof(FileSystemAttribute) == 0xC0);
    }

    void TestSignedRanges() {
        Check(!ValidateRange(-1, 1, 4), "negative offset accepted");
        Check(!ValidateRange(0, -1, 4), "negative size accepted");
        Check(ValidateRange(4, 0, 4), "zero-sized end range rejected");
        Check(!ValidateRange(4, 1, 4), "past-end range accepted");
        Check(!ValidateRange(INT64_MAX, 1, 16), "unrepresentable range accepted");
    }

    void TestGuestPathParsing() {
        std::array<u8, 8> valid{'/', 'a', '/', 'b', 0, 0xCC, 0xCC, 0xCC};
        auto path{ReadPath(valid)};
        Check(path && *path == "/a/b", "valid guest path was not preserved");

        std::array<u8, 4> traversal{'.', '.', '/', 0};
        Check(!ReadPath(traversal), "parent traversal accepted");

        std::array<u8, 4> backslash{'a', '\\', 'b', 0};
        Check(!ReadPath(backslash), "host-style separator accepted");

        std::array<u8, 4> unterminated{'a', 'b', 'c', 'd'};
        Check(!ReadPath(unterminated), "unterminated path accepted");

        std::array<u8, 0x302> oversized{};
        Check(!ReadPath(oversized), "oversized FspPath accepted");
    }
}

int main() {
    int failures{};
    const auto run = [&](const char *name, auto test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    run("filesystem ABI layouts", TestAbiLayouts);
    run("signed storage ranges", TestSignedRanges);
    run("guest path parsing", TestGuestPathParsing);
    return failures == 0 ? 0 : 1;
}
