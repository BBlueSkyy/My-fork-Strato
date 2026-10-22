#include <cstdlib>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <cassert>
#include "gpu/diagnostic/sparking_frame_capture.h"
#define LOGI(...) (void)fmt::format(__VA_ARGS__)
#define LOGE(...) (void)fmt::format(__VA_ARGS__)
namespace skyline::gpu {
using u32=std::uint32_t;
template<class T> using span=std::span<T>;
struct Staging : std::vector<std::uint8_t> {};
struct FenceCycle { template<class...T> void AttachObjects(T&&...) {} };
struct Format { vk::Format vkFormat{vk::Format::eB10G11R11UfloatPack32}; vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor}; bool IsCompressed() const {return false;} };
struct Texture {
    struct { u32 width{856},height{480},depth{1}; } dimensions;
    u32 levelCount{1},layerCount{1}; vk::SampleCountFlagBits sampleCount{vk::SampleCountFlagBits::e1};
    size_t surfaceSize{856*480*4}; Format storage; Format *format{&storage};
    vk::ImageLayout layout{vk::ImageLayout::eGeneral}; vk::ImageUsageFlags usage{vk::ImageUsageFlagBits::eTransferSrc};
    vk::Image GetBacking() {return {};}
    void CopyIntoStagingBuffer(vk::raii::CommandBuffer&,const std::shared_ptr<Staging>&) {}
};
struct TextureView { std::shared_ptr<Texture> texture; Format *format{}; vk::ImageViewType type{vk::ImageViewType::e2D}; vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1}; };
struct GPU { struct { std::shared_ptr<Staging> AllocateStagingBuffer(size_t n) {auto p=std::make_shared<Staging>();p->resize(n);return p;} } memory; };
namespace interconnect {
struct CommandExecutor {
    GPU gpu; size_t submissionNumber{7}; u32 renderPassIndex{1}; unsigned attaches{};
    std::vector<std::function<void(vk::raii::CommandBuffer&,const std::shared_ptr<FenceCycle>&,GPU&)>> commands;
    std::vector<std::function<void()>> diagnosticReadbackCallbacks;
    void AttachTexture(TextureView*) {++attaches;}
    void AddOutsideRpCommand(std::function<void(vk::raii::CommandBuffer&,const std::shared_ptr<FenceCycle>&,GPU&)>&& f) {commands.push_back(std::move(f));}
    std::optional<u32> BeginDiagnosticSparkingDraw(span<TextureView*> color);
    void EndDiagnosticSparkingDraw(u32,span<TextureView*> color);
    void QueueSparkingScanImage(TextureView*,const std::string&,u32,bool);
};
#include "executor.inc"
namespace maxwell3d {
struct Pipeline {
    struct Blend {bool blendEnable{};u32 colorWriteMask{15};};
    struct {std::array<std::uint64_t,6> shaderHashes{}; Blend GetAttachmentBlendState(int) {return {};} } sourcePackedState;
    struct Stage {
        struct Uniform {u32 index{},count{1};}; std::vector<Uniform> uniformBufferDescs{{0,1}};
        std::array<u32,16> constantBufferUsedSizes{};
        struct Image {u32 cbuf_index{},cbuf_offset{},count{1},type{};}; std::vector<Image> combinedImageSamplerDescs{{}};
    };
    struct {std::array<Stage,1> stages;} info;
    const auto &GetDiagnosticDescriptorInfo() const {return info;}
};
struct Maxwell3D {
    struct {CommandExecutor &executor;} ctx;
    struct {bool enabled{};} activeRenderCondition;
    struct {std::array<TextureView*,1> color{}; Pipeline pipeline; auto GetColorAttachments(){return span<TextureView*>{color};} auto *GetPipeline(){return &pipeline;}} activeState;
    std::vector<TextureView*> activeDescriptorSetSampledImages;
    struct {struct Bound {struct {size_t size{128};} view;}; std::array<std::array<Bound,16>,1> boundConstantBuffers;} constantBuffers;
    std::optional<u32> BeginSparkingHdrDraw(u32,u32,bool,const vk::Rect2D&);
};
#include "maxwell.inc"
}
}
}
int main() {
    using namespace skyline::gpu;
    auto root=std::filesystem::path(std::getenv("SPARKING_TEST_ROOT"))/"executor";
    std::filesystem::create_directories(root/"capture_001");
    std::ofstream(root/"ENABLE_HDR_DRAW_SCAN");
    diagnostic::SparkingCapture().Start((root/"capture_001").string());
    interconnect::CommandExecutor executor;
    auto output=std::make_shared<Texture>(),input=std::make_shared<Texture>();
    input->dimensions.width=input->dimensions.height=1;input->surfaceSize=4;
    TextureView out{output,&output->storage},in{input,&input->storage};
    interconnect::maxwell3d::Maxwell3D maxwell{{executor}};
    maxwell.activeState.color[0]=&out;maxwell.activeDescriptorSetSampledImages={&in};
    auto ordinal=maxwell.BeginSparkingHdrDraw(3,1,true,{{0,0},{856,480}});
    assert(ordinal==1);assert(executor.commands.size()==2); // baseline + first-use input
    executor.EndDiagnosticSparkingDraw(*ordinal,maxwell.activeState.GetColorAttachments());
    assert(executor.commands.size()==3 && executor.attaches==3);
    for(auto &callback:executor.diagnosticReadbackCallbacks)callback();
    assert(std::filesystem::file_size(root/"capture_001/hdr_draw_001_after.raw")==856*480*4);
    assert(std::filesystem::file_size(root/"capture_001/hdr_draw_001_input_0.raw")==4);
    assert(std::filesystem::file_size(root/"capture_001/hdr_draw_stats.csv")>0);
    auto unreadable=std::make_shared<Texture>();
    unreadable->layout=vk::ImageLayout::eShaderReadOnlyOptimal;
    TextureView skipped{unreadable,&unreadable->storage};
    maxwell.activeDescriptorSetSampledImages={&in,&skipped};
    ordinal=maxwell.BeginSparkingHdrDraw(3,1,true,{{0,0},{856,480}});
    assert(ordinal==2 && executor.commands.size()==3); // no duplicate input allocation
    diagnostic::SparkingCapture().Finish();
    assert(!executor.BeginDiagnosticSparkingDraw(maxwell.activeState.GetColorAttachments()));
    std::filesystem::remove(root/"ENABLE_HDR_DRAW_SCAN");
}
