// SPDX-License-Identifier: MPL-2.0
.text
.global fixture_version, fixture_configure, fixture_prepared, fixture_control, fixture_generate
fixture_version:
    mov w0, #1
    ret
fixture_configure:
    str wzr, [x0]
    ret
fixture_prepared:
    ret
fixture_control:
    str wzr, [x0]
    mov x0, #0
    ret
fixture_generate:
    str wzr, [x0]
    ldr x8, [x3]  // Configuration.userRx.offset
    mov w9, #0x0540
    movk w9, #0x5280, lsl #16 // mov w0, #42
    str w9, [x8]
    mov w9, #0x03c0
    movk w9, #0xd65f, lsl #16 // ret
    str w9, [x8, #4]
    mov x9, #8
    stp x8, x9, [x1]
    stp xzr, xzr, [x2]
    ldr x10, [sp, #24] // GenerateCode output buffer, twelfth AAPCS64 argument
    mov w11, #77
    str w11, [x10]
    ret
