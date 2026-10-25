// SPDX-License-Identifier: MPL-2.0
// Exact-overlap routing and alias lifecycle production methods run against
// metadata/fence facades; GPU readback is represented by an ordering assertion.
#include "host_types.h"
#include <gpu/texture/alias_group.h>
#include <gpu/texture/region_authority.h>

namespace skyline::gpu {
    using ContextTag = u64;
    struct Trap {
        void TrapRegions(int, bool) {}
    };
    struct Process { Trap trap; };
    struct GPU {
        struct {} traits;
        struct { std::shared_ptr<Process> process{std::make_shared<Process>()}; } state;
    };
    template<class Traits>
    const texture::Format *ConvertHostCompatibleFormat(const texture::Format *format, const Traits &) { return format; }
    inline bool pendingWrites{};
    inline size_t submissions{}, readbacks{};
    class Texture : public std::enable_shared_from_this<Texture> {
        std::mutex mutex;
      public:
        enum class DirtyState { Clean, CpuDirty, GpuDirty };
        GPU &gpu;
        std::shared_ptr<GuestTexture> guest;
        const texture::Format *format;
        mutable std::mutex stateMutex;
        texture::AtomicSharedPtr<texture::AliasGroup> aliasGroup;
        texture::AtomicSharedPtr<texture::RegionAuthority> regionAuthority;
        std::atomic<bool> aliasStale{}, aliasRetired{}, aliasReadOnly{}, regionStale{}, regionTracked{};
        void ReconcileRegions() { throw std::logic_error("region path belongs to real Vulkan suite"); }
        void RecordRegionWrite() { throw std::logic_error("region path belongs to real Vulkan suite"); }
        bool memoryFreed{}, replaced{}, everUsedAsGpuWrite{};
        DirtyState dirtyState{DirtyState::GpuDirty};
        vk::SampleCountFlagBits sampleCount{vk::SampleCountFlagBits::e1};
        vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
        u64 gpuWriteSerial{}, gpuReadbackSerial{};
        u32 guestMemoryPins{};
        std::optional<int> trapHandle{1};
        Texture(GPU &gpu, GuestTexture value) : gpu(gpu), guest(std::make_shared<GuestTexture>(value)), format(value.format) {}
        void lock() { mutex.lock(); }
        ContextTag pendingRegionUseTag{};
        std::atomic<ContextTag> tag{}, pendingGpuWriteTag{};
        void unlock();
        bool ViewRangeFits(vk::ImageViewType, vk::ImageSubresourceRange, u32 = 1) const { return true; }
        bool CanGrowMipLevels(u32) const { return false; }
        static bool CanMaterializeGuestLayout(const GuestTexture &);
        static bool IsGuestLayoutReversible(const GuestTexture &);
        bool CanReconcileGuestLayout(const GuestTexture &) const;
        static std::shared_ptr<texture::AliasGroup> RegisterAliases(std::vector<std::shared_ptr<Texture>>, const GuestTexture * = nullptr);
        void RequireAliasCoherence(ContextTag);
        void ReconcileAliases();
        void RecordGpuWrite();
        std::shared_ptr<void> PinGuestMemory();
        void SynchronizeGuest(bool, bool) {
            if (pendingWrites) throw std::runtime_error("readback occurred before executor submission");
            ++readbacks;
            gpuReadbackSerial = gpuWriteSerial;
            dirtyState = DirtyState::Clean;
            if (auto group{aliasGroup.load()}) group->guestGeneration = group->generation;
        }
    };
#include "alias-methods.inc"
    struct TextureManager {
        static void SynchronizeGuestOverlaps(const std::vector<std::shared_ptr<Texture>> &sources) {
            if (!sources.empty()) throw std::logic_error("exact-alias fixture cannot resolve partial readbacks");
        }
    };
    namespace interconnect {
        struct CommandExecutor {
            void Submit() { ++submissions; pendingWrites = false; }
            void HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &);
        };
#include "alias-retry.inc"
    }
    struct Selection {
        std::shared_ptr<Texture> fullMatch, unrepresentableMatch;
        std::vector<std::shared_ptr<Texture>> matches;
    };
    Selection SelectExact(const std::shared_ptr<Texture> &source, const GuestTexture &guestTexture) {
        // The exact ordered-mapping overlap is the precondition for this
        // production branch. Its complete full-match/alias/readback decision is
        // included unchanged, rather than copying its predicate into this test.
        struct { std::shared_ptr<Texture> texture; } mapping{source};
        auto *hostMapping{&mapping};
        auto &matchGuestTexture{*source->guest};
        const auto requestedStride{guestTexture.layerStride ? guestTexture.layerStride : guestTexture.CalculateLayerSize()};
        vk::ImageSubresourceRange requestedRange{};
        std::shared_ptr<Texture> fullMatch, unrepresentableMatch;
        std::vector<std::shared_ptr<Texture>> matches;
#include "alias-selection.inc"
        return {fullMatch, unrepresentableMatch, matches};
    }
    void RequestRegistration(GPU &gpu, const std::shared_ptr<Texture> &source,
                             const GuestTexture &guestTexture, ContextTag tag) {
        auto selection{SelectExact(source, guestTexture)};
        auto &unrepresentableMatch{selection.unrepresentableMatch};
        std::shared_ptr<texture::AliasGroup> separateGroup;
        std::vector<std::shared_ptr<Texture>> exactAliases{source};
        bool requestedReadOnlyAlias{};
#include "alias-registration.inc"
        (void)requestedReadOnlyAlias;
        if (!selection.matches.empty())
            source->SynchronizeGuest(false, true); // The legacy ungrouped path.
    }
}
namespace {
    using namespace skyline;
    using namespace skyline::gpu;
    using TIC = interconnect::TextureImageControl;
    void Check(bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    }
    GuestTexture Sampled(const texture::Format &format) {
        TIC tic{};
        tic.widthMinusOne = 511;
        tic.heightMinusOne = 31;
        tic.headerType = TIC::HeaderType::BlockLinear;
        tic.textureType = TIC::TextureType::e2D;
        tic.tileConfig.tileHeightGobsLog2 = 4;
        auto value{interconnect::Construct(tic, format)};
        return value;
    }
}
int main() {
    try {
        GPU gpu;
        const texture::Format rgba{1, 1, 4};
        auto spacedHeader{TIC{}};
        spacedHeader.widthMinusOne = 1023;
        spacedHeader.heightMinusOne = 255;
        spacedHeader.headerType = TIC::HeaderType::BlockLinear;
        spacedHeader.textureType = TIC::TextureType::e2D;
        spacedHeader.tileConfig.tileHeightGobsLog2 = 3;
        spacedHeader.tileConfig.sparseTileWidthGobsLog2 = 3;
        auto spaced{interconnect::Construct(spacedHeader, rgba)};
        auto unspaced{spaced};
        unspaced.tileConfig.tileWidthSpacing = 0;
        auto equivalentWriter{std::make_shared<Texture>(gpu, unspaced)};
        Check(SelectExact(equivalentWriter, spaced).fullMatch == equivalentWriter,
              "equivalent RT/TIC spacing created a separate stale backing");
        spacedHeader.widthMinusOne = 959;
        spacedHeader.heightMinusOne = 269;
        auto padded{interconnect::Construct(spacedHeader, rgba)};
        auto different{padded};
        different.tileConfig.tileWidthSpacing = 0;
        // Equal memory extents do not make different ROB encodings reusable.
        different.layerStride = padded.GetLayerStride();
        auto differentWriter{std::make_shared<Texture>(gpu, different)};
        const auto differentSelection{SelectExact(differentWriter, padded)};
        Check(!differentSelection.fullMatch && differentSelection.unrepresentableMatch == differentWriter,
              "different physical spacing bypassed coherent alias reconciliation");
        auto sampled{Sampled(rgba)};
        std::vector<u8> storage(sampled.GetSize());
        sampled.mappings.emplace_back(storage.data(), storage.size());
        auto raw{sampled};
        raw.tileConfig.blockHeight = 16;
        auto receipt{std::make_shared<Texture>(gpu, raw)};
        {
            std::scoped_lock lock{*receipt};
            receipt->tag = 77;
            receipt->RecordGpuWrite();
            Check(receipt->pendingGpuWriteTag.load() == 77, "production writer did not publish its owning executor tag");
        }
        Check(!receipt->pendingGpuWriteTag.load() && !receipt->tag.load(),
              "production unlock retained a prepared-write receipt");
        auto writer{std::make_shared<Texture>(gpu, raw)};
        writer->RecordGpuWrite();
        pendingWrites = true;
        bool requested{};
        interconnect::CommandExecutor executor;
        try {
            RequestRegistration(gpu, writer, sampled, 1);
        } catch (const TextureAliasReconciliationRequest &request) {
            requested = true;
            Check(submissions == 0 && readbacks == 0 && pendingWrites,
                  "alias registration read back pending work before retry");
            Check(request.registration.size() == 1 && request.registrationLayout &&
                  request.registrationLayout->tileConfig.blockHeight == 4,
                  "retry lost the effective sampled layout");
            Check(writer->guestMemoryPins == 1, "retry did not retain guest memory lease");
            executor.HandleTextureBackingPromotion(request);
        }
        Check(requested && submissions == 1 && readbacks == 1,
              "differing sampled Y layout did not use submission-safe alias retry");
        auto group{writer->aliasGroup.load()};
        Check(group && group->writerIdentity == writer.get() && writer->guestMemoryPins == 0,
              "writer authority or guest lease publication incorrect");
        auto cached{std::make_shared<Texture>(gpu, sampled)};
        Texture::RegisterAliases({writer, cached}, &sampled);
        cached->ReconcileAliases();
        Check(cached->aliasGroup.load() == writer->aliasGroup.load() && !cached->aliasStale.load(),
              "sampled alias did not join/reconcile writer group");
        const auto before{readbacks};
        writer->RecordGpuWrite();
        pendingWrites = true;
        Check(cached->aliasStale.load(), "future raw render-target write did not invalidate cached TIC");
        requested = false;
        try {
            cached->RequireAliasCoherence(1);
        } catch (const TextureAliasReconciliationRequest &request) {
            requested = true;
            Check(readbacks == before && pendingWrites && request.target == cached,
                  "cached alias read back before its retry submission");
            executor.HandleTextureBackingPromotion(request);
        }
        Check(requested && submissions == 2 && readbacks == before + 1 && !cached->aliasStale.load(),
              "cached sampled alias did not reconcile after later writer submission");
        // Other mismatches retain their existing routing. Same layout is reusable.
        Check(SelectExact(writer, raw).fullMatch == writer, "same-layout exact match changed");
        auto changedDepth{sampled};
        changedDepth.tileConfig.blockDepth = 2;
        Check(!SelectExact(writer, changedDepth).unrepresentableMatch,
              "Y-only route admitted a changed Z layout");
        auto changedDimensions{sampled};
        changedDimensions.dimensions.width = 256;
        Check(!SelectExact(writer, changedDimensions).unrepresentableMatch,
              "Y-only route admitted changed dimensions");
        auto changedMip{sampled};
        changedMip.mipLevelCount = 2;
        Check(!SelectExact(writer, changedMip).unrepresentableMatch,
              "Y-only route admitted a mipmapped layout");
        auto basedRaw{raw};
        basedRaw.baseArrayLayer = 1;
        auto basedWriter{std::make_shared<Texture>(gpu, basedRaw)};
        Check(!SelectExact(basedWriter, sampled).unrepresentableMatch,
              "Y-only route admitted unsupported source base array layer");
        auto basedAlias{sampled};
        basedAlias.baseArrayLayer = 1;
        Check(!SelectExact(writer, basedAlias).unrepresentableMatch,
              "Y-only route admitted unsupported requested base array layer");
        const texture::Format depthFormat{1, 1, 4, vk::ImageAspectFlagBits::eDepth};
        auto depthRaw{raw};
        depthRaw.format = &depthFormat;
        auto depthWriter{std::make_shared<Texture>(gpu, depthRaw)};
        auto depthAlias{depthRaw};
        depthAlias.tileConfig.blockHeight = 4;
        Check(!SelectExact(depthWriter, depthAlias).unrepresentableMatch,
              "Y-only route admitted unsupported depth aspect");
        std::cout << "PASS: equivalent spacing reuses backing; differing spacing reconciles aliases; later RT write invalidates cached TIC and retries\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
