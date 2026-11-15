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
    std::cout << "PASS: pinned TexturePass query specialization, bound/bindless, 8 targets; Color2D and Buffer produce different descriptor classes\n";
}
