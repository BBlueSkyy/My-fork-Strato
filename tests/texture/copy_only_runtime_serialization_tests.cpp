// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cassert>
#include <gpu/interconnect/copy_only_runtime_serialization.h>

using skyline::gpu::interconnect::CopyOnlyRuntimeSerialization;

int main() {
    CopyOnlyRuntimeSerialization first;
    CopyOnlyRuntimeSerialization second;

    assert(!first.OwnsLock());
    assert(first.TryAcquire());
    assert(first.OwnsLock());

    // Repeated acquisitions within one submission are idempotent, including
    // when the next executable route belongs to another TextureGroup.
    assert(first.TryAcquire());
    assert(!second.TryAcquire());

    first.Reset();
    assert(!first.OwnsLock());
    assert(second.TryAcquire());
    assert(second.OwnsLock());
    second.Reset();
}
