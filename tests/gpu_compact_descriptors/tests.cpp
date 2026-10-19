// SPDX-License-Identifier: MPL-2.0
#include <compare>
#include <concepts>
#include <iostream>
#include <stdexcept>
#include <shader_compiler/shader_info.h>

namespace compact {
#include "compact.inc"
}

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Compact, class Source>
void checkHandle(const Compact &actual, const Source &source) {
    static_assert(std::equality_comparable<Compact>);
    check(actual.component_type == source.component_type, "numeric type lost");
    check(actual.cbuf_index == source.cbuf_index && actual.cbuf_offset == source.cbuf_offset,
          "primary cbuf location lost");
    check(actual.has_secondary == source.has_secondary &&
          actual.secondary_cbuf_index == source.secondary_cbuf_index &&
          actual.secondary_cbuf_offset == source.secondary_cbuf_offset, "secondary cbuf location lost");
    check(actual.shift_left == source.shift_left && actual.secondary_shift_left == source.secondary_shift_left,
          "handle shifts lost");
    check(actual.mask == source.mask && actual.secondary_mask == source.secondary_mask, "handle masks lost");
    check(actual.count == source.count && actual.size_shift == source.size_shift, "descriptor array extent lost");
    check(Shader::EvaluateTextureHandle(actual, 0x12345678U, 0xABCDEF01U) ==
          Shader::EvaluateTextureHandle(source, 0x12345678U, 0xABCDEF01U), "compact handle semantics changed");
    auto different{source};
    different.component_type = source.component_type == Shader::TextureComponentType::Float
        ? Shader::TextureComponentType::Uint : Shader::TextureComponentType::Float;
    check(actual != Compact{different}, "descriptor comparison omitted numeric type");
    different = source;
    different.mask ^= 1;
    check(actual != Compact{different}, "descriptor comparison omitted mask");
}

int main() {
    for (const auto type : {Shader::TextureComponentType::Float, Shader::TextureComponentType::Uint,
                            Shader::TextureComponentType::Sint}) {
        const Shader::TextureBufferDescriptor buffer{.has_secondary = true, .component_type = type,
            .cbuf_index = 3, .cbuf_offset = 24, .shift_left = 4, .secondary_cbuf_index = 5,
            .secondary_cbuf_offset = 48, .secondary_shift_left = 20, .mask = 0xFFFF0,
            .secondary_mask = 0xFFF00000, .count = 2, .size_shift = 3};
        checkHandle(compact::UniformTexelBufferDesc{buffer}, buffer);
        for (const bool samplerless : {false, true}) {
            const Shader::TextureDescriptor texture{.type = Shader::TextureType::Color2D,
                .is_depth = false, .is_samplerless = samplerless, .component_type = type,
                .has_secondary = true, .cbuf_index = 3, .cbuf_offset = 24, .shift_left = 4,
                .secondary_cbuf_index = 5, .secondary_cbuf_offset = 48, .secondary_shift_left = 20,
                .mask = 0xFFFF0, .secondary_mask = 0xFFF00000, .count = 2, .size_shift = 3};
            const compact::CombinedImageSamplerDesc actual{texture};
            checkHandle(actual, texture);
            check(actual.type == texture.type && actual.is_samplerless == samplerless,
                  "sampled-image descriptor mode lost");
            auto changed{texture};
            changed.is_samplerless = !samplerless;
            check(actual != compact::CombinedImageSamplerDesc{changed}, "comparison omitted samplerless mode");
        }
    }
    const compact::UniformBufferDesc uniform{Shader::ConstantBufferDescriptor{.index = 2, .count = 3}};
    check(uniform.index == 2 && uniform.count == 3, "uniform buffer constructor failed");
    const compact::StorageBufferDesc storage{Shader::StorageBufferDescriptor{.cbuf_index = 2,
        .cbuf_offset = 64, .count = 3, .is_written = true, .has_size = true}};
    check(storage.cbuf_index == 2 && storage.cbuf_offset == 64 && storage.count == 3 &&
          storage.is_written && storage.has_size, "storage buffer constructor failed");
    for (const bool integer : {false, true}) {
        const compact::StorageTexelBufferDesc buffer{Shader::ImageBufferDescriptor{
            .format = Shader::ImageFormat::Typeless, .is_written = true, .is_read = true,
            .is_integer = integer, .cbuf_index = 4, .cbuf_offset = 80, .count = 1, .size_shift = 3}};
        const compact::StorageImageDesc image{Shader::ImageDescriptor{
            .type = Shader::TextureType::Color2D, .format = Shader::ImageFormat::Typeless,
            .is_written = true, .is_read = true, .is_integer = integer,
            .cbuf_index = 4, .cbuf_offset = 80, .count = 1, .size_shift = 3}};
        check(buffer.is_integer == integer && image.is_integer == integer && buffer.is_read && image.isRead &&
              buffer.is_written && image.is_written, "storage image constructor lost access or numeric flags");
    }
    std::cout << "PASS production compact descriptor constructors, comparisons and handle evaluation\n";
}
