#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <skyline/gpu/texture/copy_capability.h>

using namespace skyline::gpu::texture;

namespace {
    struct Representation {};

    ClassifiedResourceView CopyOnly(std::initializer_list<ResolvedCopySubresource> subresources) {
        return {
            .relation = TextureViewCompatibility::CopyOnly,
            .copyRegion = ResolvedCopyRegion{.subresources = subresources},
        };
    }

    constexpr ResolvedSubresource Subresource(std::uint32_t mip, std::uint32_t layer = 0,
                                               std::uint32_t depthSlice = 0) {
        return {.mip = mip, .layer = layer, .depthSlice = depthSlice};
    }
}

int main() {
    std::array<std::uint8_t, 256> memory{};
    const std::array<std::span<std::uint8_t>, 1> mapping{std::span{memory}};
    GuestResourceRanges ranges{mapping};

    const std::array<GuestSubresource, 3> subresources{
        GuestSubresource{.width = 64, .height = 32, .depth = 1, .mip = 0, .layer = 0},
        GuestSubresource{.width = 32, .height = 16, .depth = 1, .mip = 1, .layer = 0},
        GuestSubresource{.width = 64, .height = 32, .depth = 1, .mip = 0, .layer = 1},
    };
    const TextureResourceLayout layout{
        .imageType = ImageKind::TwoDimensional,
        .subresources = subresources,
    };
    const auto mip0 = Subresource(0);
    const auto mip1 = Subresource(1);
    const auto layer1 = Subresource(0, 1);
    const std::array mip0Write{mip0};

    const CopyImageInfo sourceImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .sampleCount = 1,
        .transferSource = true,
    };
    const CopyImageInfo destinationImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .sampleCount = 1,
        .transferDestination = true,
    };

    auto first = std::make_shared<Representation>();
    auto second = std::make_shared<Representation>();
    auto third = std::make_shared<Representation>();

    // A route is directional, requires a direct semantic edge, and is idempotent.
    CopyDependencyTracker<Representation> dependencies;
    assert(dependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}, {mip1, mip1}, {layer1, layer1}})));
    CopyCapabilityTracker<Representation> capabilities;
    assert(capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip0,
        second, layout, destinationImage, mip0));
    assert(capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip0,
        second, layout, destinationImage, mip0));
    assert(capabilities.RouteCount() == 1);
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip0,
        third, layout, destinationImage, mip0));

    // A current destination needs no synchronization and no redundant copy plan.
    assert(capabilities.PrepareSynchronization(dependencies, second, mip0).state ==
        CopySynchronizationState::Current);

    // A write makes the exact peer stale and the registered direction becomes ready.
    assert(dependencies.MarkWritten(first, mip0Write));
    const auto prepared = capabilities.PrepareSynchronization(dependencies, second, mip0);
    assert(prepared.state == CopySynchronizationState::Ready);
    assert(prepared.capability == CopyCapability::ExactImageCopy);
    assert(prepared.read.source == first);
    assert(prepared.read.destination == second);
    assert(prepared.read.sourceGeneration != prepared.read.destinationGeneration);

    // Scheduling reserves the exact generations, keeps metadata stale, and suppresses duplicates.
    assert(capabilities.BeginSynchronization(prepared));
    assert(dependencies.GetState(second, mip0) == CopyRepresentationState::Stale);
    assert(capabilities.PrepareSynchronization(dependencies, second, mip0).state ==
        CopySynchronizationState::Pending);

    // A failed execution never completes metadata or leaves a stale pending reservation.
    assert(!capabilities.CompleteSynchronization(dependencies, prepared, false));
    assert(dependencies.GetState(second, mip0) == CopyRepresentationState::Stale);
    assert(dependencies.GetState(second, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, layer1) == CopyRepresentationState::Current);
    const auto retried = capabilities.PrepareSynchronization(dependencies, second, mip0);
    assert(retried.state == CopySynchronizationState::Ready);
    assert(capabilities.BeginSynchronization(retried));
    assert(capabilities.CompleteSynchronization(dependencies, retried, true));
    assert(dependencies.GetState(second, mip0) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, mip1) == CopyRepresentationState::Current);
    assert(dependencies.GetState(second, layer1) == CopyRepresentationState::Current);

    // Registering A -> B does not grant B -> A.
    CopyDependencyTracker<Representation> reverseDependencies;
    assert(reverseDependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> reverseCapabilities;
    assert(reverseCapabilities.RegisterExactImageCopy(
        reverseDependencies, first, layout, sourceImage, mip0,
        second, layout, destinationImage, mip0));
    assert(reverseDependencies.MarkWritten(second, mip0Write));
    assert(reverseCapabilities.PrepareSynchronization(reverseDependencies, first, mip0).state ==
        CopySynchronizationState::CapabilityUnavailable);

    // A stale destination with no route remains unsupported.
    CopyDependencyTracker<Representation> noRouteDependencies;
    assert(noRouteDependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> noRouteCapabilities;
    assert(noRouteDependencies.MarkWritten(first, mip0Write));
    assert(noRouteCapabilities.PrepareSynchronization(noRouteDependencies, second, mip0).state ==
        CopySynchronizationState::CapabilityUnavailable);

    // A route from a stale direct neighbor is never selected as a source.
    CopyDependencyTracker<Representation> staleSourceDependencies;
    assert(staleSourceDependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    assert(staleSourceDependencies.RegisterSynchronized(second, ranges, layout, third, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> staleSourceCapabilities;
    assert(staleSourceCapabilities.RegisterExactImageCopy(
        staleSourceDependencies, second, layout, sourceImage, mip0,
        third, layout, destinationImage, mip0));
    assert(staleSourceDependencies.MarkWritten(first, mip0Write));
    assert(staleSourceCapabilities.PrepareSynchronization(staleSourceDependencies, third, mip0).state ==
        CopySynchronizationState::SourceUnavailable);

    // Writes after preparation invalidate the captured source or destination generation.
    CopyDependencyTracker<Representation> sourceRaceDependencies;
    assert(sourceRaceDependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> sourceRaceCapabilities;
    assert(sourceRaceCapabilities.RegisterExactImageCopy(
        sourceRaceDependencies, first, layout, sourceImage, mip0,
        second, layout, destinationImage, mip0));
    assert(sourceRaceDependencies.MarkWritten(first, mip0Write));
    const auto sourceRace = sourceRaceCapabilities.PrepareSynchronization(
        sourceRaceDependencies, second, mip0);
    assert(sourceRace.state == CopySynchronizationState::Ready);
    assert(sourceRaceCapabilities.BeginSynchronization(sourceRace));
    assert(sourceRaceDependencies.MarkWritten(first, mip0Write));
    assert(!sourceRaceCapabilities.CompleteSynchronization(sourceRaceDependencies, sourceRace, true));
    assert(sourceRaceDependencies.GetState(second, mip0) == CopyRepresentationState::Stale);

    CopyDependencyTracker<Representation> destinationRaceDependencies;
    assert(destinationRaceDependencies.RegisterSynchronized(first, ranges, layout, second, ranges, layout,
        CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> destinationRaceCapabilities;
    assert(destinationRaceCapabilities.RegisterExactImageCopy(
        destinationRaceDependencies, first, layout, sourceImage, mip0,
        second, layout, destinationImage, mip0));
    assert(destinationRaceDependencies.MarkWritten(first, mip0Write));
    const auto destinationRace = destinationRaceCapabilities.PrepareSynchronization(
        destinationRaceDependencies, second, mip0);
    assert(destinationRace.state == CopySynchronizationState::Ready);
    assert(destinationRaceCapabilities.BeginSynchronization(destinationRace));
    assert(destinationRaceDependencies.MarkWritten(second, mip0Write));
    assert(!destinationRaceCapabilities.CompleteSynchronization(
        destinationRaceDependencies, destinationRace, true));
    assert(destinationRaceDependencies.GetState(second, mip0) == CopyRepresentationState::Current);

    // ExactImageCopy rejects conversion, missing directional usage, multisampling, 3D, and slices.
    auto otherFormat = destinationImage;
    otherFormat.hostFormat = 44;
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip1,
        second, layout, otherFormat, mip1));
    auto noTransferSource = sourceImage;
    noTransferSource.transferSource = false;
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, noTransferSource, mip1,
        second, layout, destinationImage, mip1));
    auto multisampled = destinationImage;
    multisampled.sampleCount = 2;
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip1,
        second, layout, multisampled, mip1));
    auto threeDimensional = layout;
    threeDimensional.imageType = ImageKind::ThreeDimensional;
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, threeDimensional, sourceImage, mip1,
        second, threeDimensional, destinationImage, mip1));
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, Subresource(1, 0, 1),
        second, layout, destinationImage, Subresource(1, 0, 1)));

    auto mismatchedExtent = subresources;
    mismatchedExtent[1].width = 31;
    const TextureResourceLayout mismatchedLayout{
        .imageType = ImageKind::TwoDimensional,
        .subresources = mismatchedExtent,
    };
    assert(!capabilities.RegisterExactImageCopy(
        dependencies, first, layout, sourceImage, mip1,
        second, mismatchedLayout, destinationImage, mip1));

    // The classified 1D/height-one 2D pair becomes executable only with maintenance5.
    const std::array dimensionalSubresource{
        GuestSubresource{.offset = 0x1000, .size = 64, .width = 64, .height = 1,
                         .depth = 1, .mip = 0, .layer = 0},
    };
    const TextureResourceLayout oneDimensional{
        .tile = {.mode = TileKind::Pitch, .pitch = 64},
        .imageType = ImageKind::OneDimensional,
        .viewType = ViewKind::OneDimensional,
        .layerStride = 64,
        .viewMipCount = 1,
        .viewLayerCount = 1,
        .subresources = dimensionalSubresource,
    };
    auto heightOneTwoDimensional{oneDimensional};
    heightOneTwoDimensional.imageType = ImageKind::TwoDimensional;
    heightOneTwoDimensional.viewType = ViewKind::TwoDimensional;
    const auto dimensionalRelation = ClassifyAndResolveView(
        oneDimensional, heightOneTwoDimensional, FormatCompatibility::Exact, true);
    assert(dimensionalRelation.relation == TextureViewCompatibility::CopyOnly);

    const std::array<std::span<std::uint8_t>, 1> dimensionalMapping{
        std::span{memory}.subspan(0, 64),
    };
    const GuestResourceRanges dimensionalRanges{dimensionalMapping};
    CopyDependencyTracker<Representation> classifiedDependencies;
    assert(classifiedDependencies.RegisterSynchronized(
        first, dimensionalRanges, oneDimensional,
        second, dimensionalRanges, heightOneTwoDimensional, dimensionalRelation));
    CopyCapabilityTracker<Representation> classifiedCapabilities;
    assert(!classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, first, oneDimensional, sourceImage, mip0,
        second, heightOneTwoDimensional, destinationImage, mip0, false));
    auto reverseSourceImage = destinationImage;
    reverseSourceImage.transferSource = true;
    auto reverseDestinationImage = sourceImage;
    reverseDestinationImage.transferDestination = true;
    assert(!classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, second, heightOneTwoDimensional, reverseSourceImage, mip0,
        first, oneDimensional, reverseDestinationImage, mip0, false));
    assert(classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, first, oneDimensional, sourceImage, mip0,
        second, heightOneTwoDimensional, destinationImage, mip0, true));
    assert(classifiedCapabilities.RouteCount() == 1);
    assert(classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, second, heightOneTwoDimensional, reverseSourceImage, mip0,
        first, oneDimensional, reverseDestinationImage, mip0, true));
    assert(classifiedCapabilities.RouteCount() == 2);

    assert(classifiedDependencies.MarkWritten(first, mip0Write));
    const auto oneToTwo = classifiedCapabilities.PrepareSynchronization(
        classifiedDependencies, second, mip0);
    assert(oneToTwo.state == CopySynchronizationState::Ready);
    assert(oneToTwo.copyRegion.sourceSubresource == mip0);
    assert(oneToTwo.copyRegion.destinationSubresource == mip0);
    assert(oneToTwo.copyRegion.width == 64);
    assert(oneToTwo.copyRegion.height == 1);
    assert(oneToTwo.copyRegion.depth == 1);
    assert(oneToTwo.copyRegion.aspectMask == sourceImage.aspectMask);
    assert(classifiedCapabilities.BeginSynchronization(oneToTwo));
    assert(classifiedCapabilities.CompleteSynchronization(
        classifiedDependencies, oneToTwo, true));

    assert(classifiedDependencies.MarkWritten(second, mip0Write));
    const auto twoToOne = classifiedCapabilities.PrepareSynchronization(
        classifiedDependencies, first, mip0);
    assert(twoToOne.state == CopySynchronizationState::Ready);
    assert(twoToOne.copyRegion.sourceSubresource == mip0);
    assert(twoToOne.copyRegion.destinationSubresource == mip0);
    assert(classifiedCapabilities.BeginSynchronization(twoToOne));
    assert(classifiedCapabilities.CompleteSynchronization(
        classifiedDependencies, twoToOne, true));

    // The executable plan preserves independent source/destination mip and layer indices.
    const auto sourceMipLayer = Subresource(2, 3);
    const auto destinationMipLayer = Subresource(4, 5);
    const std::array independentSourceSubresources{
        GuestSubresource{.width = 64, .height = 1, .depth = 1, .mip = 2, .layer = 3},
    };
    const std::array independentDestinationSubresources{
        GuestSubresource{.width = 64, .height = 1, .depth = 1, .mip = 4, .layer = 5},
    };
    auto independentSourceLayout{oneDimensional};
    independentSourceLayout.subresources = independentSourceSubresources;
    auto independentDestinationLayout{heightOneTwoDimensional};
    independentDestinationLayout.subresources = independentDestinationSubresources;
    CopyDependencyTracker<Representation> independentDependencies;
    assert(independentDependencies.RegisterSynchronized(
        first, dimensionalRanges, independentSourceLayout,
        second, dimensionalRanges, independentDestinationLayout,
        CopyOnly({{sourceMipLayer, destinationMipLayer}})));
    CopyCapabilityTracker<Representation> independentCapabilities;
    assert(independentCapabilities.RegisterExactImageCopy(
        independentDependencies,
        first, independentSourceLayout, sourceImage, sourceMipLayer,
        second, independentDestinationLayout, destinationImage, destinationMipLayer, true));
    const std::array independentWrite{sourceMipLayer};
    assert(independentDependencies.MarkWritten(first, independentWrite));
    const auto independent = independentCapabilities.PrepareSynchronization(
        independentDependencies, second, destinationMipLayer);
    assert(independent.state == CopySynchronizationState::Ready);
    assert(independent.copyRegion.sourceSubresource == sourceMipLayer);
    assert(independent.copyRegion.destinationSubresource == destinationMipLayer);
    assert(independent.copyRegion.width == 64 && independent.copyRegion.height == 1 &&
        independent.copyRegion.depth == 1);

    // Other dimensional pairs and non-unit 2D heights remain unavailable.
    auto heightTwoTwoDimensional{heightOneTwoDimensional};
    auto heightTwoSubresource = dimensionalSubresource;
    heightTwoSubresource[0].height = 2;
    heightTwoTwoDimensional.subresources = heightTwoSubresource;
    assert(!classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, first, oneDimensional, sourceImage, mip0,
        second, heightTwoTwoDimensional, destinationImage, mip0, true));
    auto threeDimensionalAlias{heightOneTwoDimensional};
    threeDimensionalAlias.imageType = ImageKind::ThreeDimensional;
    assert(!classifiedCapabilities.RegisterExactImageCopy(
        classifiedDependencies, first, oneDimensional, sourceImage, mip0,
        second, threeDimensionalAlias, destinationImage, mip0, true));

    // Merge preserves direction and is idempotent.
    CopyCapabilityTracker<Representation> merged;
    merged.MergeFrom(capabilities);
    merged.MergeFrom(capabilities);
    assert(merged.RouteCount() == 1);
}
