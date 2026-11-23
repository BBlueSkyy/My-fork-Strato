// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <shader_compiler/frontend/ir/ir_emitter.h>
namespace Shader::Log {
void Debug(const std::string&) {} void Warn(const std::string&) {} void Error(const std::string&) {}
}
int main(int argc, char** argv) {
    using namespace Shader;
    assert(argc == 2);
    std::filesystem::create_directories(argv[1]);
    for (auto stage : {Stage::Compute, Stage::Fragment}) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool}; IR::Program program;
    program.stage = stage;
    program.workgroup_size = {1, 1, 1};
    program.info.used_storage_buffer_types = IR::Type::U32;
    program.info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    program.blocks = {&block}; program.post_order_blocks = {&block};
    program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    IR::IREmitter ir{block};
    for (u32 n = 0; n < 3; ++n) {
        TextureBufferDescriptor descriptor{};
        descriptor.count = 1;
#ifndef TEXEL_BASELINE
        descriptor.numeric_type = static_cast<TextureBufferType>(n);
#endif
        program.info.texture_buffer_descriptors.push_back(descriptor);
        IR::TextureInstInfo info{};
        info.type.Assign(TextureType::Buffer);
        info.descriptor_index.Assign(n);
        if (n != 0) info.relaxed_precision.Assign(1);
        auto fetch = ir.ImageFetch(IR::Value{}, ir.Imm32(0u), IR::Value{}, IR::U32{}, IR::U32{}, info);
        fetch.Inst()->ReplaceOpcode(IR::Opcode::ImageFetch);
        for (u32 component = 0; component < 4; ++component) {
            auto bits = ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(fetch, component)});
            if (stage == Stage::Compute)
                block.PrependNewInst(block.end(), IR::Opcode::WriteStorage32,
                    {ir.Imm32(0u), ir.Imm32((n*4+component)*4), bits});
            else if (component == 0) {
                ir.SetFragColor(0,n,IR::F32{ir.CompositeExtract(fetch,component)});
                program.info.stores_frag_color[0] = true;
            }
        }
    }
    Profile profile{}; profile.supported_spirv = 0x00010300; profile.unified_descriptor_binding = true;
    const auto words = Backend::SPIRV::EmitSPIRV(profile, program);
    std::ofstream file{std::filesystem::path(argv[1]) / (stage == Stage::Compute ? "mixed.spv" : "mixed-fragment.spv"), std::ios::binary};
    file.write(reinterpret_cast<const char*>(words.data()), words.size()*4);
    assert(file.good());
    }
}
