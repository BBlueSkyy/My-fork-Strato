// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>
#include "gpu/device_fault.h"

using namespace skyline::gpu;
namespace {
    std::atomic<unsigned> calls{};
    VkResult countResult{VK_SUCCESS}, infoResult{VK_SUCCESS};
    uint32_t addressCount{1}, vendorCount{1};

    VKAPI_ATTR VkResult VKAPI_CALL GetFault(VkDevice, VkDeviceFaultCountsEXT *counts, VkDeviceFaultInfoEXT *info) {
        ++calls;
        assert(counts->sType == VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT);
        assert(!counts->pNext);
        if (!info) {
            counts->addressInfoCount = addressCount;
            counts->vendorInfoCount = vendorCount;
            counts->vendorBinarySize = 0;
            return countResult;
        }
        assert(info->sType == VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT);
        assert(!info->pNext);
        assert(counts->addressInfoCount <= DeviceFaultReport::MaxEntries);
        assert(counts->vendorInfoCount <= DeviceFaultReport::MaxEntries);
        assert(counts->vendorBinarySize == 0 && !info->pVendorBinaryData);
        assert(bool(info->pAddressInfos) == bool(counts->addressInfoCount));
        assert(bool(info->pVendorInfos) == bool(counts->vendorInfoCount));
        std::strcpy(info->description, "test fault");
        if (counts->addressInfoCount)
            info->pAddressInfos[0] = {VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x1234, 64};
        if (counts->vendorInfoCount) {
            std::strcpy(info->pVendorInfos[0].description, "vendor detail");
            info->pVendorInfos[0].vendorFaultCode = 7;
            info->pVendorInfos[0].vendorFaultData = 8;
        }
        return infoResult;
    }
}
int main() {
    // Disabling the feature must prevent entry into the driver and must not consume the one-shot guard.
    DeviceFaultDiagnostics diagnostics;
    assert(!diagnostics.CaptureAfterDeviceLoss(false, {}, GetFault));
    assert(calls == 0);
    auto report = diagnostics.CaptureAfterDeviceLoss(true, {}, GetFault);
    assert(report && report->result == VK_SUCCESS && calls == 2);
    assert(std::strcmp(report->description.data(), "test fault") == 0);
    assert(report->addressCount == 1 && report->addresses[0].reportedAddress == 0x1234);
    assert(report->vendorCount == 1 && report->vendors[0].vendorFaultCode == 7);
    assert(!diagnostics.CaptureAfterDeviceLoss(true, {}, GetFault) && calls == 2);

    // Driver query failure cannot trigger a second query or replace the original device-loss exception.
    calls = 0;
    countResult = VK_ERROR_UNKNOWN;
    DeviceFaultDiagnostics failed;
    report = failed.CaptureAfterDeviceLoss(true, {}, GetFault);
    assert(report && report->result == VK_ERROR_UNKNOWN && calls == 1);
    assert(report->addressCount == 0 && report->vendorCount == 0);
    countResult = VK_SUCCESS;

    // Bound hostile/large counts; VK_INCOMPLETE is useful partial data, never a retry loop.
    calls = 0;
    addressCount = vendorCount = std::numeric_limits<uint32_t>::max();
    infoResult = VK_INCOMPLETE;
    DeviceFaultDiagnostics incomplete;
    report = incomplete.CaptureAfterDeviceLoss(true, {}, GetFault);
    assert(report && report->result == VK_INCOMPLETE && calls == 2 && report->truncated);
    assert(report->addressCount == DeviceFaultReport::MaxEntries);
    assert(report->vendorCount == DeviceFaultReport::MaxEntries);

    // Empty reports still retrieve the description and provide null zero-length arrays.
    calls = 0;
    addressCount = vendorCount = 0;
    infoResult = VK_SUCCESS;
    DeviceFaultDiagnostics empty;
    report = empty.CaptureAfterDeviceLoss(true, {}, GetFault);
    assert(report && report->result == VK_SUCCESS && calls == 2);
    assert(report->addressCount == 0 && report->vendorCount == 0);
    assert(std::strcmp(report->description.data(), "test fault") == 0);

    // Failure of the second driver call must not expose partially written output as a valid report.
    calls = 0;
    addressCount = vendorCount = 1;
    infoResult = VK_ERROR_UNKNOWN;
    DeviceFaultDiagnostics retrievalFailure;
    report = retrievalFailure.CaptureAfterDeviceLoss(true, {}, GetFault);
    assert(report && report->result == VK_ERROR_UNKNOWN && calls == 2);
    assert(report->addressCount == 0 && report->vendorCount == 0);
    infoResult = VK_SUCCESS;

    // A missing entry point is handled without dereferencing it.
    DeviceFaultDiagnostics missing;
    report = missing.CaptureAfterDeviceLoss(true, {}, nullptr);
    assert(report && report->result == VK_ERROR_EXTENSION_NOT_PRESENT);

    // Multiple failing threads issue only one two-call report query.
    calls = 0;
    DeviceFaultDiagnostics concurrent;
    std::atomic<unsigned> reports{};
    auto capture = [&] { if (concurrent.CaptureAfterDeviceLoss(true, {}, GetFault)) ++reports; };
    std::thread a(capture), b(capture), c(capture);
    a.join(); b.join(); c.join();
    assert(calls == 2 && reports == 1);
}
