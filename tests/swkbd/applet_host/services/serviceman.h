// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <test_host.h>
// Tests call the real IPC handlers directly; dispatch table lookup is outside this fixture.
#define SERVICE_DECL(...)
#include <skyline/services/am/applet/indirect_layer_registry.h>
