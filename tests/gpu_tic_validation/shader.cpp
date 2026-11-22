// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <iostream>
#include <shader_compiler/environment.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
#include <shader_compiler/host_translate_info.h>
#include <shader_compiler/ir_opt/passes.h>
#include <shader_compiler/common/log.h>
namespace Shader::Log {
void Debug(const std::string&) {} void Warn(const std::string&) {} void Error(const std::string&) {}
}
struct TestEnvironment : Shader::Environment {
    Shader::TextureType type; int queries{};
    explicit TestEnvironment(Shader::TextureType t) : type(t) { stage = Shader::Stage::Fragment; }
    u64 ReadInstruction(u32) override { throw std::runtime_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { ++queries; return type; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { throw std::runtime_error("unexpected pixel format"); }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    Shader::CompareFunction ReadTextureCompareFunc(u32) override { throw std::runtime_error("unexpected compare"); }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64) override {}
};
// Reproduce a split handle with sampler word in c[16][0] through the pinned
// TexturePass. No shader code or binding is invented by the runtime fallback.
struct SplitHandleEnvironment : TestEnvironment {
    bool secondaryBound{};
    u32 lastHandle{};
    Shader::TextureHandleSource source{};
    SplitHandleEnvironment() : TestEnvironment(Shader::TextureType::Color2D) {}
    u32 ReadCbufValue(u32 index, u32 offset) override {
        assert(offset == 0);
        assert(index == 2 || index == 16);
        return index == 2 ? 7u : (secondaryBound ? 3u : 0u);
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        lastHandle = handle;
        return TestEnvironment::ReadTextureType(handle);
    }
    u32 QueryTexture(const Shader::TextureHandleSource& input, Shader::TextureQueryKind kind) override {
        source = input;
        return Shader::Environment::QueryTexture(input, kind);
    }
};
int main() {
    using namespace Shader;
    for (bool bindless : {false, true}) {
        for (TextureType type : {TextureType::Color1D, TextureType::ColorArray1D, TextureType::Color2D,
                TextureType::ColorArray2D, TextureType::Color3D, TextureType::ColorCube,
                TextureType::ColorArrayCube, TextureType::Buffer}) {
            ObjectPool<IR::Inst> instructions; ObjectPool<IR::Block> blocks; IR::Program program;
            auto block = blocks.Create(instructions);
            program.blocks = {block}; program.post_order_blocks = {block};
            IR::IREmitter ir{*block};
            auto handle = bindless ? IR::Value{ir.GetCbuf(ir.Imm32(2), ir.Imm32(0))} : IR::Value{u32{0}};
            auto query = ir.ImageQueryDimension(handle, ir.Imm32(0));
            TestEnvironment env{type}; HostTranslateInfo host{};
            Optimization::TexturePass(env, program, host);
            assert(env.queries > 0);
            assert(query.Inst()->GetOpcode() == IR::Opcode::ImageQueryDimensions);
            assert(query.Inst()->Flags<IR::TextureInstInfo>().type == type);
            if (type == TextureType::Buffer) {
                assert(program.info.texture_buffer_descriptors.size() == 1);
                assert(program.info.texture_descriptors.empty());
            } else {
                assert(program.info.texture_descriptors.size() == 1);
                assert(program.info.texture_descriptors[0].type == type);
                assert(program.info.texture_buffer_descriptors.empty());
            }
        }
    }
    for (bool secondaryBound : {false, true}) {
        ObjectPool<IR::Inst> instructions; ObjectPool<IR::Block> blocks; IR::Program program;
        auto block = blocks.Create(instructions);
        program.blocks = {block}; program.post_order_blocks = {block};
        IR::IREmitter ir{*block};
        auto primary = ir.GetCbuf(ir.Imm32(2), ir.Imm32(0));
        auto secondary = ir.GetCbuf(ir.Imm32(16), ir.Imm32(0));
        auto handle = ir.BitwiseOr(primary, ir.ShiftLeftLogical(secondary, ir.Imm32(20)));
        ir.ImageQueryDimension(handle, ir.Imm32(0));
        SplitHandleEnvironment env;
        env.secondaryBound = secondaryBound;
        HostTranslateInfo host{};
        Optimization::TexturePass(env, program, host);
        assert(program.info.texture_descriptors.size() == 1);
        const auto& desc = program.info.texture_descriptors[0];
        assert(desc.has_secondary && desc.cbuf_index == 2 && desc.cbuf_offset == 0);
        assert(desc.secondary_cbuf_index == 16 && desc.secondary_cbuf_offset == 0);
        assert(desc.shift_left == 0 && desc.secondary_shift_left == 20);
        assert(env.source.cbuf_index == desc.cbuf_index && env.source.secondary_cbuf_index == 16);
        assert(env.source.secondary_shift_left == desc.secondary_shift_left);
        assert(env.lastHandle == (7u | (secondaryBound ? 3u << 20 : 0u)));
    }
    std::cout << "PASS: split c[2]/c[16] handle origins and metadata, secondary absent/bound\n";
    std::cout << "PASS: pinned TexturePass query specialization, bound/bindless, 8 targets; Color2D and Buffer produce different descriptor classes\n";
}
