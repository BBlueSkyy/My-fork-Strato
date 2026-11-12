// SPDX-License-Identifier: MPL-2.0
// Compile actual generated probes with pinned Vulkan/fmt and host metadata facades.
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "trace.h"
#define LOGI(...) static_cast<void>(fmt::format(__VA_ARGS__))
using u32 = uint32_t;
namespace skyline::gpu {
namespace texture {
enum class TileMode { Block, Pitch };
struct Tile { TileMode mode{TileMode::Block}; uint8_t blockHeight{16}, blockDepth{1}; };
}
struct Format { vk::Format vkFormat{vk::Format::eB10G11R11UfloatPack32}; };
struct Guest {
    vk::Extent3D dimensions{320,180,1};
    u32 mipLevelCount{5}; texture::Tile tileConfig;
};
struct Parent {
    vk::Extent3D dimensions{320,180,1};
    std::shared_ptr<Guest> guest{std::make_shared<Guest>()};
    std::shared_ptr<Format> format{std::make_shared<Format>()};
    u32 levelCount{5};
    void Promotion() {
        const u32 nextLevels{5}; const bool admitFormat{};
#include "promotion-probe.inc"
    }
    void Reconcile() {
        const u32 generation{1}; const bool download{}; std::shared_ptr<Parent> writer;
#include "reconcile-probe.inc"
    }
};
using Texture = Parent;
struct TextureView {
    std::shared_ptr<Parent> texture;
    std::shared_ptr<Format> format;
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,1,1,0,1};
    vk::ComponentMapping mapping;
};
struct Blend {
    bool blendEnable{true};
    vk::BlendFactor srcColorBlendFactor{vk::BlendFactor::eOne};
    vk::BlendFactor dstColorBlendFactor{vk::BlendFactor::eConstantColor};
};
struct Pipeline {
    struct {
        std::array<uint64_t,6> shaderHashes{0,0,0,0,0,0x05EB6F31815A3BF8ULL};
        std::array<Blend,8> attachmentBlendStates;
    } sourcePackedState;
};
struct State {
    Pipeline pipeline;
    std::vector<TextureView *> attachments;
    const Pipeline *GetPipeline() const { return &pipeline; }
    const std::vector<TextureView *> &GetColorAttachments() const { return attachments; }
};
static void CheckProbes() {
    auto parent{std::make_shared<Parent>()};
    Guest guestTexture;
    std::array<uint8_t,16> bytes{}; std::span<uint8_t> guestMapping{bytes};
    vk::ImageSubresourceRange requestedRange{vk::ImageAspectFlagBits::eColor,0,5,0,1};
#include "manager-probe.inc"
    pr341Trace("TEST", parent, 1, 0);
    parent->Promotion(); parent->Reconcile();
    std::vector<std::shared_ptr<Parent>> textures{parent};
    const Guest *requested{nullptr};
#include "alias-probe.inc"
    State activeState;
    TextureView sample{parent}, target{parent};
    activeState.attachments = {&target};
    std::vector<TextureView *> activeDescriptorSetSampledImages{&sample,nullptr};
    vk::Rect2D scissor{{0,0},{160,90}};
    vk::PipelineStageFlags srcStageMask{vk::PipelineStageFlagBits::eColorAttachmentOutput};
    vk::PipelineStageFlags dstStageMask{vk::PipelineStageFlagBits::eFragmentShader};
    std::array<float,4> pr341BlendConsts{0.1f,0.2f,0.3f,1.0f};
    struct Settings { std::shared_ptr<bool> freeGuestTextureMemory{std::make_shared<bool>(false)}; };
    struct { struct { struct { std::shared_ptr<Settings> settings{std::make_shared<Settings>()}; } state; } gpu; } ctx;
#include "draw-probe.inc"
}
}
int main() { skyline::gpu::CheckProbes(); }
