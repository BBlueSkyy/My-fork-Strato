#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <skyline/gpu/texture/storage.h>

using namespace skyline::gpu::texture;

int main() {
    std::array<std::uint8_t, 64> memory{};
    const std::array<std::span<std::uint8_t>, 1> mapping{std::span{memory}};
    GuestResourceRanges ranges{mapping};
    const std::array subresources{
        GuestSubresource{.width = 8, .height = 8, .depth = 1},
    };
    const TextureResourceLayout layout{
        .imageType = ImageKind::TwoDimensional,
        .subresources = subresources,
    };
    const ResolvedSubresource mip0{};
    const ClassifiedResourceView copyOnly{
        .relation = TextureViewCompatibility::CopyOnly,
        .copyRegion = ResolvedCopyRegion{.subresources = {{{}, {}}}},
    };
    const CopyImageInfo sourceImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .transferSource = true,
    };
    const CopyImageInfo destinationImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .transferDestination = true,
    };

    auto group = std::make_shared<TextureGroup>();
    auto source = std::make_shared<TextureStorage>(nullptr, group, ranges);
    auto destination = std::make_shared<TextureStorage>(nullptr, group, ranges);
    group->Attach(source);
    group->Attach(destination);
    assert(group->RegisterSynchronizedCopyDependency(
        source, layout, destination, layout, copyOnly));
    assert(group->RegisterExactImageCopyCapability(
        source, layout, sourceImage, mip0,
        destination, layout, destinationImage, mip0));
    assert(group->PrepareCopySynchronization(destination, mip0).state ==
        CopySynchronizationState::Current);

    const std::array write{mip0};
    assert(group->MarkCopyRepresentationWritten(source, write));
    const auto prepared = group->PrepareCopySynchronization(destination, mip0);
    assert(prepared.state == CopySynchronizationState::Ready);
    assert(!group->CompleteCopySynchronization(prepared, false));
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Stale);
    assert(group->CompleteCopySynchronization(prepared, true));
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Current);
}

