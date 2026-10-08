#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
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
}
