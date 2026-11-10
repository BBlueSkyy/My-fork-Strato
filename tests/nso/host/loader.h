// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
#include <vfs/backing.h>
#include <loader/executable.h>
namespace skyline::loader {
inline std::optional<Executable> capturedExecutable;
class Loader {
public:
    struct ExecutableLoadInfo { u8 *base; size_t size; void *entry; };
    virtual ~Loader() = default;
    virtual void *LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &, const DeviceState &) = 0;
    ExecutableLoadInfo LoadExecutable(const std::shared_ptr<kernel::type::KProcess> &, const DeviceState &, Executable &executable, size_t, const std::string &, bool) {
        capturedExecutable = executable;
        return {};
    }
};
}
