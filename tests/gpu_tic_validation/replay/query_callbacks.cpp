// SPDX-License-Identifier: GPL-2.0-or-later
// Focused contract test for compiler Environment texture metadata queries.
#include <cassert>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <shader_compiler/environment.h>

using namespace Shader;
struct LegacyEnvironment : Environment {
    using Address = std::pair<u32, u32>;
    std::map<Address, u32> words{{{2, 0x20}, 0x123}, {{5, 0x80}, 0x45}};
    std::vector<std::tuple<std::string, u32, u32>> events;
    u32 expected_handle{};
    u64 ReadInstruction(u32) override { assert(false); return 0; }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        events.emplace_back("cbuf", bank, offset);
        return words.at({bank, offset});
    }
    void QueryEvent(const char* kind, u32 handle) {
        events.emplace_back(kind, handle, 0);
        assert(handle == expected_handle);
    }
    TextureType ReadTextureType(u32 handle) override {
        QueryEvent("type", handle); return TextureType::Color2D;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        QueryEvent("format", handle); return TexturePixelFormat::OTHER;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        QueryEvent("integer", handle); return true;
    }
    TextureBufferType ReadTextureBufferType(u32 handle) override {
        QueryEvent("buffer-type", handle); return TextureBufferType::Sint;
    }
    CompareFunction ReadTextureCompareFunc(u32 handle) override {
        QueryEvent("compare", handle); return CompareFunction::Less;
    }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64) override {}
};
struct TrackingEnvironment final : LegacyEnvironment {
    std::vector<std::pair<TextureHandleSource, TextureQueryKind>> origins;
    u32 QueryTexture(const TextureHandleSource& source, TextureQueryKind kind) override {
        origins.emplace_back(source, kind);
        // A renderer can read handles through its own accessor without marking
        // their raw values as scalar constants that were folded into shader code.
        const u32 handle = (words.at({source.cbuf_index, source.cbuf_offset}) << source.shift_left) |
            (words.at({source.secondary_cbuf_index, source.secondary_cbuf_offset}) << source.secondary_shift_left);
        switch (kind) {
        case TextureQueryKind::Type: return static_cast<u32>(ReadTextureType(handle));
        case TextureQueryKind::PixelFormat: return static_cast<u32>(ReadTexturePixelFormat(handle));
        case TextureQueryKind::Integer: return IsTexturePixelFormatInteger(handle);
        case TextureQueryKind::BufferType: return static_cast<u32>(ReadTextureBufferType(handle));
        case TextureQueryKind::Compare: return static_cast<u32>(ReadTextureCompareFunc(handle));
        }
        assert(false); return 0;
    }
};
int main() {
    struct Case { TextureQueryKind kind; const char* event; u32 result; };
    for (const auto& c : {Case{TextureQueryKind::Type, "type", static_cast<u32>(TextureType::Color2D)},
                         Case{TextureQueryKind::PixelFormat, "format", static_cast<u32>(TexturePixelFormat::OTHER)},
                         Case{TextureQueryKind::Integer, "integer", 1},
                         Case{TextureQueryKind::BufferType, "buffer-type", static_cast<u32>(TextureBufferType::Sint)},
                         Case{TextureQueryKind::Compare, "compare", static_cast<u32>(CompareFunction::Less)}}) {
        for (const auto source : {TextureHandleSource{2, 0x20, 7, 5, 0x80, 20},
                                  TextureHandleSource{2, 0x20, 0, 5, 0x80, 0},
                                  TextureHandleSource{2, 0x20, 0, 2, 0x20, 0},
                                  TextureHandleSource{2, 0x20, 31, 5, 0x80, 0}}) {
            LegacyEnvironment legacy; TrackingEnvironment tracking;
            legacy.expected_handle = tracking.expected_handle =
                (0x123u << source.shift_left) |
                ((source.secondary_cbuf_index == 2 ? 0x123u : 0x45u) << source.secondary_shift_left);
            assert(legacy.QueryTexture(source, c.kind) == c.result);
            assert(tracking.QueryTexture(source, c.kind) == c.result);
            assert(legacy.events.size() == 3);
            assert(legacy.events[0] == std::make_tuple(std::string("cbuf"), source.cbuf_index, source.cbuf_offset));
            assert(legacy.events[1] == std::make_tuple(std::string("cbuf"), source.secondary_cbuf_index, source.secondary_cbuf_offset));
            assert(legacy.events[2] == std::make_tuple(std::string(c.event), legacy.expected_handle, 0u));
            assert(tracking.events.size() == 1 && tracking.events[0] == legacy.events[2]);
            assert(tracking.origins.size() == 1 && tracking.origins[0].second == c.kind);
            const auto& got = tracking.origins[0].first;
            assert(got.cbuf_index == source.cbuf_index && got.cbuf_offset == source.cbuf_offset && got.shift_left == source.shift_left);
            assert(got.secondary_cbuf_index == source.secondary_cbuf_index &&
                   got.secondary_cbuf_offset == source.secondary_cbuf_offset && got.secondary_shift_left == source.secondary_shift_left);
        }
    }
}
