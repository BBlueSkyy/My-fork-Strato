#include <cassert>

#include "gpu/maintenance5_support.h"

using skyline::gpu::Maintenance5Support;

int main() {
    constexpr Maintenance5Support complete{
        .createRenderpass2Extension = true,
        .depthStencilResolveExtension = true,
        .dynamicRenderingExtension = true,
        .maintenance5Extension = true,
        .maintenance5Feature = true,
    };

    assert(complete.CanEnable());

    auto missingCreateRenderpass2{complete};
    missingCreateRenderpass2.createRenderpass2Extension = false;
    assert(!missingCreateRenderpass2.CanEnable());

    auto missingDepthStencilResolve{complete};
    missingDepthStencilResolve.depthStencilResolveExtension = false;
    assert(!missingDepthStencilResolve.CanEnable());

    auto missingDynamicRendering{complete};
    missingDynamicRendering.dynamicRenderingExtension = false;
    assert(!missingDynamicRendering.CanEnable());

    auto missingMaintenance5{complete};
    missingMaintenance5.maintenance5Extension = false;
    assert(!missingMaintenance5.CanEnable());

    auto missingMaintenance5Feature{complete};
    missingMaintenance5Feature.maintenance5Feature = false;
    assert(!missingMaintenance5Feature.CanEnable());
}
