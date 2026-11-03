// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <sstream>
#include <string>
#include <vector>
#include <gpu/texture/spongebob_mini.h>

using skyline::gpu::texture::diagnostic::SpongeBobMini;
using skyline::gpu::texture::diagnostic::SpongeBobMiniInfo;

static SpongeBobMiniInfo Info(uint64_t operation, uint32_t role, uint8_t value) {
    (void)value;
    return SpongeBobMiniInfo{
        .operation=operation, .role=role,
        .image=42, .generation=7, .writeSerial=15,
        .hostFormat=37, .guestFormat=37, .width=512, .height=512,
        .x=224, .y=224, .regionWidth=64, .regionHeight=64,
        .mip=0, .layer=0, .blockWidth=1, .blockHeight=1,
        .bytesPerBlock=4, .size=64*64*4,
        .pipelineHash=0x1234, .channelSequence=345,
        .shaderHashes={0x11,0x22,0x33,0x44,0x55,0x66},
        .drawKind=1, .topology=3, .indexed=1,
        .first=0, .count=6, .instances=1
    };
}

int main() {
    SpongeBobMini mini;
    assert(mini.Arm());
    assert(!mini.Arm());
    const auto op{mini.Begin()};
    assert(op == 1 && !mini.Armed());
    std::ostringstream empty;
    assert(!mini.Export(empty));

    const auto black{std::vector<uint8_t>(64*64*4, 0)};
    const auto white{std::vector<uint8_t>(64*64*4, 255)};
    assert(mini.Add(Info(op,1,0),[&] { return black; }));
    assert(mini.Add(Info(op,3,0),[&] { return black; }));
    assert(!mini.Add(Info(op,3,0),[&] { return white; }));
    std::ostringstream incomplete;
    assert(!mini.Export(incomplete)); // Not frozen, may still receive target-after.
    assert(mini.Add(Info(op,2,255),[&] {
        // Reader callback must execute with controller mutex released.
        assert(!mini.Armed());
        return white;
    }));
    // The natural render pass must be closed before the post-pass payload
    // is eligible for export, even if all snapshots were queued.
    mini.EndNaturalPass();
    std::ostringstream complete;
    assert(mini.Export(complete));
    assert(complete.str().starts_with("SBMINI3\n1,1,42,7,15,37,37,"));
    assert(complete.str().size() > black.size() * 3);
    assert(!mini.Arm()); // Frozen after export.
    assert(!mini.Add(Info(op,4,0),[&] { return black; }));
    std::ostringstream again;
    assert(mini.Export(again) && again.str() == complete.str());

    SpongeBobMini cancelled;
    assert(cancelled.Arm());
    const auto op2{cancelled.Begin()};
    assert(cancelled.Add(Info(op2,1,0),[&] { return black; }));
    assert(cancelled.Add(Info(op2,3,0),[&] { return black; }));
    assert(cancelled.Add(Info(op2,2,0),[] { return std::vector<uint8_t>{}; }));
    cancelled.EndNaturalPass();
    std::ostringstream invalid;
    assert(!cancelled.Export(invalid));
    assert(invalid.str().empty()); // Never export an incomplete payload.

    SpongeBobMini excessive;
    for (unsigned i{};i<3;++i) {
        assert(excessive.Arm());
        assert(excessive.Begin()==i+1);
        excessive.EndNaturalPass();
    }
    assert(!excessive.Arm());
}
