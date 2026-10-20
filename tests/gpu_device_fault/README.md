# Optional Vulkan device-fault diagnostics

Run with initialized `app/libraries/vkhpp` and its `Vulkan-Headers` submodule:

```sh
python3 tests/gpu_device_fault/run.py
python3 tests/gpu_device_fault/compile_vulkan.py
```

`VULKAN_HEADERS` can override the C header include directory for `run.py`;
`VULKAN_HPP` can override the Vulkan-Hpp checkout for `compile_vulkan.py`.

The sanitizer test exercises the production collector using a simulated Vulkan
entry point: disabled feature, once-only concurrent capture, count/query errors,
empty reports, missing entry point, and bounded `VK_INCOMPLETE` output. It does not
create a device or reproduce a GPU fault.

The pinned Vulkan-Hpp test compiles the production `CreateDevice` and reporting
methods. It executes the feature-negotiation portion of the actual trait
constructor (stopping before unrelated physical-device format/memory queries),
covering all combinations of NV extension advertisement and geometry support.
It also exercises the actual raw device-fault feature query, and extracts the
production fence wait method to verify typed device-loss propagation and existing
retry cases. Android interfaces and driver operations are host stubs. These checks
are not a full Android build or real Qualcomm driver validation.

`VK_EXT_device_fault` is queried only when advertised; its `deviceFault` feature
and extension are then enabled together. The pinned Vulkan-Hpp predates the
extension, so `device_fault_compat.h` supplies only the ABI declarations needed by
the raw query, behind the Vulkan extension guard. Vendor binary generation is
left disabled. The reporter is called only after an observed device-loss error;
it makes at most one count query and one bounded data query per device. It adds
no Vulkan waits, submissions, threads, polling, or sleep. Successful draws and
presentation do not call it.

The report records driver-provided descriptions, address type/address/precision,
and vendor codes/data. It can help investigate a future device loss; it does not
establish that a prior shader warning caused a particular loss. The diagnostic
coverage includes queue submission, the presentation thread, and the command
fence waiter. Other device-loss sources that never propagate through these paths
may still terminate without collecting a report.

References:

- https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceFaultInfoEXT.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFaultFeaturesEXT.html
