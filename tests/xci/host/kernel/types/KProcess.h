// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <vfs/backing.h>
namespace skyline::vfs {
// Process setup is outside these container/storage tests. This shim ensures an
// accidental call fails instead of pretending the guest NPDM was validated.
struct NPDM {
    NPDM() = default;
    explicit NPDM(const std::shared_ptr<Backing> &) { throw exception("NPDM host shim cannot load a process"); }
};
}
namespace skyline::kernel::type { struct KProcess { vfs::NPDM npdm; }; }
