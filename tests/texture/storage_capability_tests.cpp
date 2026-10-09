#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
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

    // Runtime serialization is local to one alias group: unrelated CopyOnly
    // relations must remain independently lockable.
    auto independentGroup = std::make_shared<TextureGroup>();
    std::unique_lock groupRuntimeLock{group->RuntimeSynchronizationMutex()};
    std::unique_lock independentRuntimeLock{
        independentGroup->RuntimeSynchronizationMutex(), std::try_to_lock};
    assert(independentRuntimeLock.owns_lock());
    std::unique_lock duplicateGroupRuntimeLock{
        group->RuntimeSynchronizationMutex(), std::try_to_lock};
    assert(!duplicateGroupRuntimeLock.owns_lock());
    independentRuntimeLock.unlock();
    groupRuntimeLock.unlock();

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
    assert(group->BeginCopySynchronization(prepared));
    assert(group->PrepareCopySynchronization(destination, mip0).state ==
        CopySynchronizationState::Pending);
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Stale);
    assert(!group->CompleteCopySynchronization(prepared, false));
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Stale);
    const auto retried = group->PrepareCopySynchronization(destination, mip0);
    assert(retried.state == CopySynchronizationState::Ready);
    assert(group->BeginCopySynchronization(retried));
    assert(group->CompleteCopySynchronization(retried, true));
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Current);

    // A runtime ticket owns the reservation until a real submit completes. Dropping
    // it before submission cancels only the pending state and never promotes metadata.
    assert(group->MarkCopyRepresentationWritten(source, write));
    auto abandoned = group->ScheduleCopySynchronization(destination, mip0);
    assert(abandoned.state == CopySynchronizationState::Ready && abandoned.pending);
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Stale);
    assert(group->ScheduleCopySynchronization(destination, mip0).state ==
        CopySynchronizationState::Pending);
    abandoned.pending.reset();
    auto submitted = group->ScheduleCopySynchronization(destination, mip0);
    assert(submitted.state == CopySynchronizationState::Ready && submitted.pending);
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Stale);
    assert(submitted.pending->Complete());
    assert(group->GetCopyRepresentationState(destination, mip0) ==
        CopyRepresentationState::Current);

    // Group migration is deferred while a post-fence reservation is outstanding.
    assert(group->MarkCopyRepresentationWritten(source, write));
    auto inFlight = group->ScheduleCopySynchronization(destination, mip0);
    assert(inFlight.state == CopySynchronizationState::Ready && inFlight.pending);
    auto blockedMerge = std::make_shared<TextureGroup>();
    assert(!blockedMerge->TryMergeCopyDependenciesFrom(*group));
    assert(source->GetGroup() == group && destination->GetGroup() == group);
    assert(!inFlight.pending->Complete(false));
    assert(blockedMerge->TryMergeCopyDependenciesFrom(*group));
    assert(source->GetGroup() == blockedMerge && destination->GetGroup() == blockedMerge);

    // Group merge imports a usable directional route and remains idempotent.
    auto importedGroup = std::make_shared<TextureGroup>();
    auto importedSource = std::make_shared<TextureStorage>(nullptr, importedGroup, ranges);
    auto importedDestination = std::make_shared<TextureStorage>(nullptr, importedGroup, ranges);
    importedGroup->Attach(importedSource);
    importedGroup->Attach(importedDestination);
    assert(importedGroup->RegisterSynchronizedCopyDependency(
        importedSource, layout, importedDestination, layout, copyOnly));
    assert(importedGroup->RegisterExactImageCopyCapability(
        importedSource, layout, sourceImage, mip0,
        importedDestination, layout, destinationImage, mip0));

    auto mergedGroup = std::make_shared<TextureGroup>();
    assert(mergedGroup->TryMergeCopyDependenciesFrom(*importedGroup));
    assert(mergedGroup->TryMergeCopyDependenciesFrom(*importedGroup));
    assert(importedSource->GetGroup() == mergedGroup);
    assert(importedDestination->GetGroup() == mergedGroup);

    assert(mergedGroup->MarkCopyRepresentationWritten(importedSource, write));
    const auto imported = mergedGroup->PrepareCopySynchronization(importedDestination, mip0);
    assert(imported.state == CopySynchronizationState::Ready);
    assert(mergedGroup->BeginCopySynchronization(imported));
    assert(mergedGroup->CompleteCopySynchronization(imported, true));
    assert(mergedGroup->GetCopyRepresentationState(importedDestination, mip0) ==
        CopyRepresentationState::Current);

    assert(mergedGroup->MarkCopyRepresentationWritten(importedDestination, write));
    assert(mergedGroup->PrepareCopySynchronization(importedSource, mip0).state ==
        CopySynchronizationState::CapabilityUnavailable);

    // Production registration is transactional: an unsupported pair records no dependency.
    const std::array dimensionalSubresources{
        GuestSubresource{.width = 8, .height = 1, .depth = 1},
    };
    const TextureResourceLayout oneDimensional{
        .imageType = ImageKind::OneDimensional,
        .subresources = dimensionalSubresources,
    };
    const TextureResourceLayout twoDimensional{
        .imageType = ImageKind::TwoDimensional,
        .subresources = dimensionalSubresources,
    };
    auto unsupportedGroup = std::make_shared<TextureGroup>();
    auto unsupportedSource = std::make_shared<TextureStorage>(nullptr, unsupportedGroup, ranges);
    auto unsupportedDestination = std::make_shared<TextureStorage>(nullptr, unsupportedGroup, ranges);
    unsupportedGroup->Attach(unsupportedSource);
    unsupportedGroup->Attach(unsupportedDestination);
    auto unsupportedSourceImage{sourceImage};
    unsupportedSourceImage.aspectMask = 3;
    auto unsupportedDestinationImage{destinationImage};
    unsupportedDestinationImage.aspectMask = 3;
    assert(!unsupportedGroup->RegisterMaintenance5CopyOnly(
        unsupportedSource, oneDimensional, unsupportedSourceImage,
        unsupportedDestination, twoDimensional, unsupportedDestinationImage, copyOnly));
    assert(unsupportedGroup->GetCopyRepresentationState(unsupportedSource, mip0) ==
        CopyRepresentationState::Untracked);

    // A partially executable pair is not activated: each direction is validated
    // independently, but the undirected semantic dependency requires both routes.
    auto directionalGroup = std::make_shared<TextureGroup>();
    auto directionalSource = std::make_shared<TextureStorage>(nullptr, directionalGroup, ranges);
    auto directionalDestination = std::make_shared<TextureStorage>(nullptr, directionalGroup, ranges);
    directionalGroup->Attach(directionalSource);
    directionalGroup->Attach(directionalDestination);
    assert(!directionalGroup->RegisterMaintenance5CopyOnly(
        directionalSource, oneDimensional, sourceImage,
        directionalDestination, twoDimensional, destinationImage, copyOnly));
    assert(directionalGroup->GetCopyRepresentationState(directionalSource, mip0) ==
        CopyRepresentationState::Untracked);
    assert(directionalGroup->GetCopyRepresentationState(directionalDestination, mip0) ==
        CopyRepresentationState::Untracked);

    // A cube view whose existing 2D backing lacks the Vulkan cube-compatible flag
    // receives exact, independently validated routes in both directions.
    std::array<GuestSubresource, 6> cubeSubresources{};
    std::vector<ResolvedCopySubresource> cubeMappings;
    for (std::uint32_t layer{}; layer < cubeSubresources.size(); ++layer) {
        cubeSubresources[layer] = {
            .offset = layer * 64,
            .size = 64,
            .width = 8,
            .height = 8,
            .depth = 1,
            .mip = 0,
            .layer = layer,
        };
        cubeMappings.push_back({
            .backing = {.mip = 0, .layer = layer},
            .requested = {.mip = 0, .layer = layer},
        });
    }
    std::array<std::uint8_t, 384> cubeMemory{};
    const std::array<std::span<std::uint8_t>, 1> cubeMapping{std::span{cubeMemory}};
    GuestResourceRanges cubeRanges{cubeMapping};
    const TextureResourceLayout nonCubeLayout{
        .imageType = ImageKind::TwoDimensional,
        .viewType = ViewKind::TwoDimensionalArray,
        .cubeCompatible = false,
        .layerStride = 64,
        .viewMipCount = 1,
        .viewLayerCount = 6,
        .subresources = cubeSubresources,
    };
    auto cubeLayout{nonCubeLayout};
    cubeLayout.viewType = ViewKind::Cube;
    cubeLayout.cubeCompatible = true;
    const ClassifiedResourceView cubeCopyOnly{
        .relation = TextureViewCompatibility::CopyOnly,
        .copyRegion = ResolvedCopyRegion{.subresources = cubeMappings},
    };
    const CopyImageInfo bidirectionalImage{
        .hostFormat = 37,
        .aspectMask = 1,
        .sampleCount = 1,
        .transferSource = true,
        .transferDestination = true,
    };
    auto cubeGroup = std::make_shared<TextureGroup>();
    auto nonCubeStorage = std::make_shared<TextureStorage>(nullptr, cubeGroup, cubeRanges);
    auto cubeStorage = std::make_shared<TextureStorage>(nullptr, cubeGroup, cubeRanges);
    cubeGroup->Attach(nonCubeStorage);
    cubeGroup->Attach(cubeStorage);
    assert(cubeGroup->RegisterCubeCompatibleCopyOnly(
        nonCubeStorage, nonCubeLayout, bidirectionalImage,
        cubeStorage, cubeLayout, bidirectionalImage,
        cubeCopyOnly, FormatCompatibility::Exact));

    const ResolvedSubresource layer3{.mip = 0, .layer = 3};
    const std::array nonCubeWrite{layer3};
    assert(cubeGroup->MarkCopyRepresentationWritten(nonCubeStorage, nonCubeWrite));
    auto toCube = cubeGroup->PrepareCopySynchronization(cubeStorage, layer3);
    assert(toCube.state == CopySynchronizationState::Ready);
    assert(cubeGroup->BeginCopySynchronization(toCube));
    assert(cubeGroup->CompleteCopySynchronization(toCube, true));

    const ResolvedSubresource layer4{.mip = 0, .layer = 4};
    const std::array cubeWrite{layer4};
    assert(cubeGroup->MarkCopyRepresentationWritten(cubeStorage, cubeWrite));
    auto fromCube = cubeGroup->PrepareCopySynchronization(nonCubeStorage, layer4);
    assert(fromCube.state == CopySynchronizationState::Ready);
    assert(cubeGroup->BeginCopySynchronization(fromCube));
    assert(cubeGroup->CompleteCopySynchronization(fromCube, true));

    auto cubeArrayLayout{cubeLayout};
    cubeArrayLayout.viewType = ViewKind::CubeArray;
    auto cubeArrayGroup = std::make_shared<TextureGroup>();
    auto arrayBacking = std::make_shared<TextureStorage>(nullptr, cubeArrayGroup, cubeRanges);
    auto arrayRequested = std::make_shared<TextureStorage>(nullptr, cubeArrayGroup, cubeRanges);
    cubeArrayGroup->Attach(arrayBacking);
    cubeArrayGroup->Attach(arrayRequested);
    assert(cubeArrayGroup->RegisterCubeCompatibleCopyOnly(
        arrayBacking, nonCubeLayout, bidirectionalImage,
        arrayRequested, cubeArrayLayout, bidirectionalImage,
        cubeCopyOnly, FormatCompatibility::Exact));

    // Invalid shape/format/host capability combinations remain transactional.
    const auto expectCubeRegistrationRejected = [&](const TextureResourceLayout &backingLayout,
                                                     const CopyImageInfo &backingImage,
                                                     const TextureResourceLayout &requestedLayout,
                                                     const CopyImageInfo &requestedImage,
                                                     FormatCompatibility format) {
        auto rejectedGroup = std::make_shared<TextureGroup>();
        auto rejectedBacking = std::make_shared<TextureStorage>(nullptr, rejectedGroup, cubeRanges);
        auto rejectedRequested = std::make_shared<TextureStorage>(nullptr, rejectedGroup, cubeRanges);
        rejectedGroup->Attach(rejectedBacking);
        rejectedGroup->Attach(rejectedRequested);
        assert(!rejectedGroup->RegisterCubeCompatibleCopyOnly(
            rejectedBacking, backingLayout, backingImage,
            rejectedRequested, requestedLayout, requestedImage,
            cubeCopyOnly, format));
        assert(rejectedGroup->GetCopyRepresentationState(rejectedBacking, mip0) ==
            CopyRepresentationState::Untracked);
    };
    expectCubeRegistrationRejected(nonCubeLayout, bidirectionalImage, cubeLayout,
                                   bidirectionalImage, FormatCompatibility::ViewCompatible);
    auto nonCubeRequestedLayout{cubeLayout};
    nonCubeRequestedLayout.cubeCompatible = false;
    expectCubeRegistrationRejected(nonCubeLayout, bidirectionalImage, nonCubeRequestedLayout,
                                   bidirectionalImage, FormatCompatibility::Exact);
    auto mismatchedFormatImage{bidirectionalImage};
    mismatchedFormatImage.hostFormat = 44;
    expectCubeRegistrationRejected(nonCubeLayout, bidirectionalImage, cubeLayout,
                                   mismatchedFormatImage, FormatCompatibility::Exact);
    auto oneWayImage{bidirectionalImage};
    oneWayImage.transferDestination = false;
    expectCubeRegistrationRejected(nonCubeLayout, bidirectionalImage, cubeLayout,
                                   oneWayImage, FormatCompatibility::Exact);
    auto multisampledImage{bidirectionalImage};
    multisampledImage.sampleCount = 2;
    expectCubeRegistrationRejected(nonCubeLayout, multisampledImage, cubeLayout,
                                   multisampledImage, FormatCompatibility::Exact);
}
