// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/pitch_linear.h>
#include <gpu/texture/bc_decoder.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <span>
#include <vector>

int main() {
    using namespace skyline::gpu::texture;
    constexpr size_t layers{3};
    constexpr std::array<std::uint16_t, layers> colors{0xF800, 0x07E0, 0x001F};
    constexpr std::array<std::array<std::uint8_t, 4>, layers> expected{{
        {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}
    }};
    size_t cases{};
    for (size_t bytes : {8U, 16U})
        for (size_t width : {1U, 5U, 8U})
            for (size_t height : {1U, 5U, 7U, 8U}) {
                const auto rowBytes{((width + 3) / 4) * bytes};
                const auto layout{GetPitchLinearLayout(width, height, 4, 4, bytes, rowBytes + 32)};
                std::vector<std::uint8_t> pitched(layout.layerSize * layers, 0xEA);
                std::vector<std::uint8_t> packed(layout.linearSize * layers);
                std::vector<std::uint8_t> output(width * height * 4 * layers, 0xCC);
                for (size_t layer{}; layer < layers; ++layer) {
                    for (size_t row{}; row < layout.rowCount; ++row)
                        for (size_t column{}; column < (width + 3) / 4; ++column) {
                            auto block{pitched.data() + layer * layout.layerSize + row * (rowBytes + 32) + column * bytes};
                            std::fill_n(block, bytes, 0);
                            if (bytes == 16) { block[0] = 255; block[1] = 255; block += 8; }
                            block[0] = block[2] = static_cast<std::uint8_t>(colors[layer]);
                            block[1] = block[3] = static_cast<std::uint8_t>(colors[layer] >> 8);
                        }
                    CopyPitchedRows(std::span{pitched}.subspan(layer * layout.layerSize, layout.layerSize), rowBytes + 32,
                        std::span{packed}.subspan(layer * layout.linearSize, layout.linearSize), rowBytes, rowBytes, layout.rowCount);
                }
                DecodePitchLinearLayers(packed, layout.linearSize, output, width * height * 4, layers,
                    [&](const std::uint8_t *input, std::uint8_t *destination) {
                        if (bytes == 8) bcn::DecodeBc1(input, destination, width, height, true);
                        else bcn::DecodeBc3(input, destination, width, height);
                    });
                for (size_t layer{}; layer < layers; ++layer)
                    for (size_t pixel{}; pixel < width * height; ++pixel)
                        for (size_t channel{}; channel < 4; ++channel)
                            assert(output[(layer * width * height + pixel) * 4 + channel] == expected[layer][channel]);
                ++cases;
            }
    std::cout << "PASS: " << cases << " BC1/BC3 pitched upload + CPU decode cases\n";
}
