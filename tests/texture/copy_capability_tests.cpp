#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <skyline/gpu/texture/copy_capability.h>
#include <skyline/gpu/texture/copy_format_compatibility.h>

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

    // CopyCompatible capability discovery is directional and remains separate
    // from route registration in this checkpoint.
    const auto forwardFormat = ClassifyHostFormatCompatibility(
        vk::Format::eR32Uint, vk::ImageAspectFlagBits::eColor,
        vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, false);
    const auto reverseFormat = ClassifyHostFormatCompatibility(
        vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
        vk::Format::eR32Uint, vk::ImageAspectFlagBits::eColor, false);
    assert(forwardFormat == FormatCompatibility::CopyCompatible);
    assert(reverseFormat == FormatCompatibility::CopyCompatible);
    auto copyCompatibleSource = sourceImage;
    copyCompatibleSource.hostFormat = static_cast<std::uint64_t>(
        static_cast<VkFormat>(vk::Format::eR32Uint));
    auto copyCompatibleDestination = destinationImage;
    copyCompatibleDestination.hostFormat = static_cast<std::uint64_t>(
        static_cast<VkFormat>(vk::Format::eR8G8B8A8Unorm));
    assert(CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, copyCompatibleDestination, layer1,
        forwardFormat));

    auto reverseCopySource = copyCompatibleDestination;
    reverseCopySource.transferSource = true;
    auto reverseCopyDestination = copyCompatibleSource;
    reverseCopyDestination.transferDestination = true;
    assert(CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, reverseCopySource, layer1,
        layout, reverseCopyDestination, mip0,
        reverseFormat));

    auto missingDirectionalUsage = copyCompatibleSource;
    missingDirectionalUsage.transferSource = false;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, missingDirectionalUsage, mip0,
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::Exact));

    auto mismatchedAspect = copyCompatibleDestination;
    mismatchedAspect.aspectMask = 2;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, mismatchedAspect, layer1,
        FormatCompatibility::CopyCompatible));
    auto multipleAspects = copyCompatibleDestination;
    multipleAspects.aspectMask = 3;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, multipleAspects, layer1,
        FormatCompatibility::CopyCompatible));
    auto depthAspectSource = copyCompatibleSource;
    auto depthAspectDestination = copyCompatibleDestination;
    depthAspectSource.aspectMask = 2;
    depthAspectDestination.aspectMask = 2;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, depthAspectSource, mip0,
        layout, depthAspectDestination, layer1,
        FormatCompatibility::CopyCompatible));

    auto mismatchedSamples = copyCompatibleDestination;
    mismatchedSamples.sampleCount = 2;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, mismatchedSamples, layer1,
        FormatCompatibility::CopyCompatible));
    auto multisampledSource = copyCompatibleSource;
    multisampledSource.sampleCount = 2;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, multisampledSource, mip0,
        layout, mismatchedSamples, layer1,
        FormatCompatibility::CopyCompatible));

    auto oneDimensionalCopyLayout = layout;
    oneDimensionalCopyLayout.imageType = ImageKind::OneDimensional;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        oneDimensionalCopyLayout, copyCompatibleSource, mip0,
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));
    auto threeDimensionalCopyLayout = layout;
    threeDimensionalCopyLayout.imageType = ImageKind::ThreeDimensional;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        threeDimensionalCopyLayout, copyCompatibleSource, mip0,
        threeDimensionalCopyLayout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));

    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, Subresource(7),
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, Subresource(0, 0, 1),
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip1,
        layout, copyCompatibleDestination, layer1,
        FormatCompatibility::CopyCompatible));
    auto exactHostFormat = copyCompatibleDestination;
    exactHostFormat.hostFormat = copyCompatibleSource.hostFormat;
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, exactHostFormat, layer1,
        FormatCompatibility::CopyCompatible));

    auto incompatibleHostFormat = copyCompatibleDestination;
    incompatibleHostFormat.hostFormat = static_cast<std::uint64_t>(
        static_cast<VkFormat>(vk::Format::eR16G16B16A16Unorm));
    assert(!CopyCapabilityTracker<Representation>::SupportsCopyCompatibleImageCopy(
        layout, copyCompatibleSource, mip0,
        layout, incompatibleHostFormat, layer1,
        FormatCompatibility::CopyCompatible));

    // CopyCompatible routes reuse ExactImageCopy execution metadata while preserving
    // directional registration, Pending, and generation completion semantics.
    auto copyCompatibleFirstImage = copyCompatibleSource;
    copyCompatibleFirstImage.transferDestination = true;
    auto copyCompatibleSecondImage = copyCompatibleDestination;
    copyCompatibleSecondImage.transferSource = true;
    CopyDependencyTracker<Representation> copyCompatibleDependencies;
    assert(copyCompatibleDependencies.RegisterSynchronized(
        first, ranges, layout, second, ranges, layout, CopyOnly({{mip0, mip0}})));
    CopyCapabilityTracker<Representation> copyCompatibleCapabilities;
    assert(copyCompatibleCapabilities.RegisterCopyCompatibleImageCopy(
        copyCompatibleDependencies,
        first, layout, copyCompatibleFirstImage, mip0,
        second, layout, copyCompatibleSecondImage, mip0,
        FormatCompatibility::CopyCompatible));
    assert(copyCompatibleCapabilities.RegisterCopyCompatibleImageCopy(
        copyCompatibleDependencies,
        second, layout, copyCompatibleSecondImage, mip0,
        first, layout, copyCompatibleFirstImage, mip0,
        FormatCompatibility::CopyCompatible));
    assert(copyCompatibleCapabilities.RouteCount() == 2);

    assert(copyCompatibleDependencies.MarkWritten(first, mip0Write));
    auto copyCompatibleForward = copyCompatibleCapabilities.PrepareSynchronization(
        copyCompatibleDependencies, second, mip0);
    assert(copyCompatibleForward.state == CopySynchronizationState::Ready);
    assert(copyCompatibleForward.capability == CopyCapability::ExactImageCopy);
    assert(copyCompatibleCapabilities.BeginSynchronization(copyCompatibleForward));
    assert(copyCompatibleCapabilities.PrepareSynchronization(
        copyCompatibleDependencies, second, mip0).state ==
        CopySynchronizationState::Pending);
    assert(copyCompatibleCapabilities.CompleteSynchronization(
        copyCompatibleDependencies, copyCompatibleForward, true));
    assert(copyCompatibleDependencies.GetState(second, mip0) ==
        CopyRepresentationState::Current);

    assert(copyCompatibleDependencies.MarkWritten(second, mip0Write));
    auto copyCompatibleReverse = copyCompatibleCapabilities.PrepareSynchronization(
        copyCompatibleDependencies, first, mip0);
    assert(copyCompatibleReverse.state == CopySynchronizationState::Ready);
    assert(copyCompatibleCapabilities.BeginSynchronization(copyCompatibleReverse));
    assert(copyCompatibleCapabilities.CompleteSynchronization(
        copyCompatibleDependencies, copyCompatibleReverse, true));
    assert(copyCompatibleDependencies.GetState(first, mip0) ==
        CopyRepresentationState::Current);

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

    // Vulkan 1.1 permits an exact image copy between one 3D Z slice and one
    // 2D layer. The route preserves independent mip/layer/slice identity and
    // turns only the 3D side's depthSlice into offset.z.
    std::array<std::uint8_t, 256> depthMemory{};
    const auto depthAddress = reinterpret_cast<std::uintptr_t>(depthMemory.data());
    const std::array<std::span<std::uint8_t>, 1> volumeMapping{
        std::span{depthMemory},
    };
    const std::array<std::span<std::uint8_t>, 1> sliceMapping{
        std::span{depthMemory}.subspan(64, 64),
    };
    const std::array volumeSubresources{
        GuestSubresource{
            .offset = depthAddress,
            .size = 256,
            .width = 8,
            .height = 8,
            .depth = 2,
            .mip = 3,
            .layer = 0,
            .blockHeight = 1,
            .blockDepth = 2,
            .depthSlices = {
                GuestDepthSlice{.size = 64, .segments = {{depthAddress, 64, 0}}},
                GuestDepthSlice{.size = 64, .segments = {{depthAddress + 64, 64, 0}}},
            },
        },
    };
    const std::array sliceSubresources{
        GuestSubresource{
            .offset = depthAddress + 64,
            .size = 64,
            .width = 8,
            .height = 8,
            .depth = 1,
            .mip = 5,
            .layer = 2,
            .blockHeight = 1,
            .blockDepth = 2,
            .depthSlices = {
                GuestDepthSlice{.size = 64, .segments = {{depthAddress + 64, 64, 0}}},
            },
        },
    };
    const TextureResourceLayout volumeLayout{
        .tile = {.mode = TileKind::Block, .blockHeight = 1, .blockDepth = 2},
        .imageType = ImageKind::ThreeDimensional,
        .viewType = ViewKind::ThreeDimensional,
        .layerStride = 256,
        .viewMipBase = 3,
        .viewMipCount = 1,
        .viewLayerCount = 1,
        .subresources = volumeSubresources,
    };
    const TextureResourceLayout sliceLayout{
        .tile = {.mode = TileKind::Block, .blockHeight = 1, .blockDepth = 2},
        .imageType = ImageKind::TwoDimensional,
        .viewType = ViewKind::TwoDimensional,
        .layerStride = 64,
        .viewMipBase = 5,
        .viewMipCount = 1,
        .viewLayerBase = 2,
        .viewLayerCount = 1,
        .subresources = sliceSubresources,
    };
    const auto depthRelation = ClassifyAndResolveView(
        volumeLayout, sliceLayout, FormatCompatibility::Exact, true);
    assert(depthRelation.relation == TextureViewCompatibility::CopyOnly);
    const auto volumeSlice = Subresource(3, 0, 1);
    const auto surfaceLayer = Subresource(5, 2, 0);
    CopyDependencyTracker<Representation> depthDependencies;
    assert(depthDependencies.RegisterSynchronized(
        first, GuestResourceRanges{volumeMapping}, volumeLayout,
        second, GuestResourceRanges{sliceMapping}, sliceLayout, depthRelation));
    CopyCapabilityTracker<Representation> depthCapabilities;
    const CopyImageInfo bidirectionalImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .sampleCount = 1,
        .transferSource = true,
        .transferDestination = true,
    };
    assert(depthCapabilities.RegisterExactImageCopy(
        depthDependencies,
        first, volumeLayout, bidirectionalImage, volumeSlice,
        second, sliceLayout, bidirectionalImage, surfaceLayer));
    assert(depthCapabilities.RegisterExactImageCopy(
        depthDependencies,
        second, sliceLayout, bidirectionalImage, surfaceLayer,
        first, volumeLayout, bidirectionalImage, volumeSlice));

    const std::array volumeWrite{volumeSlice};
    assert(depthDependencies.MarkWritten(first, volumeWrite));
    const auto volumeToSlice = depthCapabilities.PrepareSynchronization(
        depthDependencies, second, surfaceLayer);
    assert(volumeToSlice.state == CopySynchronizationState::Ready);
    assert(volumeToSlice.copyRegion.sourceImageType == ImageKind::ThreeDimensional);
    assert(volumeToSlice.copyRegion.destinationImageType == ImageKind::TwoDimensional);
    assert(volumeToSlice.copyRegion.sourceOffsetZ == 1);
    assert(volumeToSlice.copyRegion.destinationOffsetZ == 0);
    assert(volumeToSlice.copyRegion.depth == 1);
    assert(depthCapabilities.BeginSynchronization(volumeToSlice));
    assert(depthCapabilities.CompleteSynchronization(
        depthDependencies, volumeToSlice, true));

    const std::array surfaceWrite{surfaceLayer};
    assert(depthDependencies.MarkWritten(second, surfaceWrite));
    const auto sliceToVolume = depthCapabilities.PrepareSynchronization(
        depthDependencies, first, volumeSlice);
    assert(sliceToVolume.state == CopySynchronizationState::Ready);
    assert(sliceToVolume.copyRegion.sourceImageType == ImageKind::TwoDimensional);
    assert(sliceToVolume.copyRegion.destinationImageType == ImageKind::ThreeDimensional);
    assert(sliceToVolume.copyRegion.sourceOffsetZ == 0);
    assert(sliceToVolume.copyRegion.destinationOffsetZ == 1);
    assert(sliceToVolume.copyRegion.depth == 1);

    auto wrongSliceLayout{sliceLayout};
    auto wrongSliceSubresources{sliceSubresources};
    wrongSliceSubresources[0].width = 7;
    wrongSliceLayout.subresources = wrongSliceSubresources;
    assert(!depthCapabilities.RegisterExactImageCopy(
        depthDependencies,
        first, volumeLayout, bidirectionalImage, volumeSlice,
        second, wrongSliceLayout, bidirectionalImage, surfaceLayer));
    assert(!depthCapabilities.RegisterExactImageCopy(
        depthDependencies,
        first, volumeLayout, bidirectionalImage, Subresource(3, 0, 2),
        second, sliceLayout, bidirectionalImage, surfaceLayer));
}
