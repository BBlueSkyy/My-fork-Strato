// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <sstream>
#include <string>
#include <vector>
#include <gpu/texture/spongebob_mini.h>

using skyline::gpu::texture::diagnostic::SpongeBobMini;
using skyline::gpu::texture::diagnostic::SpongeBobMiniInfo;
using skyline::gpu::texture::diagnostic::SpongeBobWriter;

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
    std::ostringstream stillOpen;
    assert(!mini.Export(stillOpen)); // Complete entries must still wait for natural RP end.
    assert(stillOpen.str().empty());
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

    // The scene filter should be explicit and reversible. Skipped overlay
    // candidates must not consume a snapshot slot or start a render pass.
    SpongeBobMini selection;
    assert(selection.Arm(false));
    assert(!selection.PreferScenePass());
    assert(selection.SkippedOverlayPass() == 1);
    assert(selection.SkippedOverlayPass() == 2);
    assert(selection.Begin() == 1);
    selection.EndNaturalPass();
    assert(selection.Arm(true));
    assert(selection.PreferScenePass());
    assert(selection.SkippedOverlayPass() == 1); // reset on re-arm
    assert(selection.Begin() == 2);
    selection.EndNaturalPass();

    // Writer history is passive, bounded and never records after disarm.
    SpongeBobMini producer;
    SpongeBobWriter key{.textureKey = 42, .generation = 7,
                         .guestFirstBegin = 0x10000, .guestFirstSize = 0x4000};
    producer.NotePossibleWriter(key);
    assert(producer.RecordedPossibleWriters() == 0);
    assert(producer.Arm());
    auto writer{key};
    writer.pipeline = 0xABCD;
    writer.vertexShader = 0x1100;
    writer.fragmentShader = 0x2200;
    writer.renderPassIndex = 8;
    writer.kind = 1;
    producer.NotePossibleWriter(writer);
    auto found{producer.FindPriorWriters(key)};
    assert(found.matchingEvents == 1 && found.exactTexture && !found.truncated);
    assert(found.latest.pipeline == 0xABCD && found.latest.ordinal == 1);

    // Alias matching is weaker than exact texture/generation matching.
    auto alias{key};
    alias.textureKey = 99;
    alias.generation = 1;
    alias.guestFirstBegin = 0x11000;
    alias.guestFirstSize = 0x2000;
    found = producer.FindPriorWriters(alias);
    assert(found.matchingEvents == 1 && !found.exactTexture);
    alias.guestFirstBegin = 0x50000;
    assert(!producer.FindPriorWriters(alias).matchingEvents);

    // After beginning capture, the previous writer history is frozen.
    assert(producer.Begin() == 1);
    producer.NotePossibleWriter(writer);
    assert(producer.RecordedPossibleWriters() == 1);
    producer.EndNaturalPass();
    assert(producer.Arm());
    assert(producer.RecordedPossibleWriters() == 0); // Fresh arm = new provenance window.
    for (unsigned i{}; i < 260; ++i) producer.NotePossibleWriter(writer);
    found = producer.FindPriorWriters(key);
    assert(!found.truncated && found.matchingEvents == 260);
    assert(found.earliest.ordinal == 1 && found.latest.ordinal == 260);
    assert(found.identifiedEvents == 260 && found.latestIdentified.ordinal == 260);
    assert(producer.TrackedResources() == 1 && producer.EvictedResources() == 0);

    // A last shader-identified writer survives many later shaderless clears,
    // unlike a global 256-event ring buffer.
    SpongeBobMini pinned;
    assert(pinned.Arm());
    pinned.NotePossibleWriter(writer);
    auto clear{writer};
    clear.pipeline = clear.vertexShader = clear.fragmentShader = 0;
    clear.kind = 7;
    for (unsigned i{}; i < 2000; ++i) pinned.NotePossibleWriter(clear);
    found = pinned.FindPriorWriters(key);
    assert(!found.truncated && found.exactTexture && found.matchingEvents == 2001);
    assert(found.latest.kind == 7 && found.latest.ordinal == 2001);
    assert(found.identifiedEvents == 1);
    assert(found.latestIdentified.ordinal == 1 &&
           found.latestIdentified.fragmentShader == 0x2200);

    // DMA has no Texture identity; guest mapping overlap is a weaker hint.
    SpongeBobMini dma;
    assert(dma.Arm());
    SpongeBobWriter memoryWriter{};
    memoryWriter.guestFirstBegin = 0x11000;
    memoryWriter.guestFirstSize = 0x2000;
    memoryWriter.kind = 9;
    dma.NotePossibleWriter(memoryWriter);
    found = dma.FindPriorWriters(key);
    assert(found.matchingEvents == 1 && !found.exactTexture);
    assert(found.latest.kind == 9 && !found.identifiedEvents);

    // Capacity is limited by distinct resources, not number of writes.
    SpongeBobMini bounded;
    assert(bounded.Arm());
    SpongeBobWriter newest{};
    for (unsigned i{}; i < 520; ++i) {
        newest = key;
        newest.textureKey = 0x80000 + i * 16;
        newest.guestFirstBegin = 0x400000 + i * 0x2000;
        newest.guestFirstSize = 0x1000;
        bounded.NotePossibleWriter(newest);
    }
    assert(bounded.RecordedPossibleWriters() == 520);
    assert(bounded.TrackedResources() == 512);
    assert(bounded.EvictedResources() == 8);
    found = bounded.FindPriorWriters(newest);
    assert(found.exactTexture && found.truncated && found.matchingEvents == 1);
    assert(found.latest.ordinal == 520 && found.resourcesEvicted == 8);
}
