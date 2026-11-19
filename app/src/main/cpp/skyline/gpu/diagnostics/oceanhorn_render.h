// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only telemetry for PR #469. Guard inclusion with STRATO_OCEAN_VIS_MODE == 1.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <logger/logger.h>

namespace skyline::gpu::diagnostics::oceanhorn {
    struct Events {
        static inline std::atomic<uint64_t> newTextures{}, newTextureBytes{};
        static inline std::atomic<uint64_t> newBuffers{}, newBufferBytes{};
        static inline std::atomic<uint64_t> bufferWrites{}, bufferWriteBytes{}, bufferCopyBytes{};
        static inline std::atomic<uint64_t> sampledNullTextures{}, guestRejectedDraws{};
        // A sampled TIC fallback is not the same thing as an intentionally
        // unbound bindless descriptor. Keep reasons and sentinels separate.
        static inline std::atomic<uint64_t> fallbackPool{}, fallbackFormat{}, fallbackUnmapped{}, fallbackInvalid{};
        static inline std::atomic<uint64_t> nullBindlessSampled{}, nullBindlessStorage{}, tscOutOfRange{};
        static inline std::atomic<uint64_t> samplersCreated{}, samplerMipDisabled{}, samplerClampZero{};
        static inline std::atomic<uint64_t> samplerMinPositive{}, samplerBiasNonzero{}, samplerAnisotropic{};
        static inline std::atomic<uint64_t> guestToHostUploads{}, guestToHostBytes{}, hostToGuestReadbacks{}, hostToGuestBytes{};
        static inline std::atomic<uint64_t> ticBuilt{}, ticBaseMipNonzero{};
        static inline std::atomic<uint64_t> mipGrowths{}, mipLevelsAdded{};
    };

    struct DrawObservation {
        uint64_t shaderKey{}, materialKey{}, elements{};
        uint32_t instances{}, textureSlots{}, nullSlots{}, baseMipNonzero{}, partialMipViews{};
        uint32_t mip0{}, mip1{}, mip2{}, mip3Plus{}, singleMip{}, mipOutOfRange{};
        uint32_t path{}; // 0 direct, 1 indirect, 2 inline-index, 3 DRAW_TEXTURE
        bool indexed{}, zeroScissor{}, hasColor{}, hasDepth{};
        bool depthTest{}, depthWrite{}, alphaTest{}, rasterDiscard{}, cull{}, predicate{};
    };

    class RenderSummary {
        struct Signature { uint64_t key{}, uses{}; };
        struct Window {
            uint64_t draws{}, direct{}, indexed{}, indirect{}, inlineIndex{}, drawTexture{};
            uint64_t elements{}, empty{}, noColor{}, noDepth{}, scissorZero{};
            uint64_t depthTest{}, depthWrite{}, alphaTest{}, rasterDiscard{}, cull{}, predicate{};
            uint64_t textureSlots{}, nullSlots{}, baseMip{}, partialMip{}, extraSignatures{};
            uint64_t mip0{}, mip1{}, mip2{}, mip3Plus{}, singleMip{}, mipOutOfRange{}, extraMaterials{};
            std::array<Signature, 32> signatures{}, materials{};
        } window{};
        uint64_t lastSubmission{}, reportSerial{};
        std::array<uint64_t, 9> previous{};
        std::array<uint64_t, 21> previousDetails{};

        static std::array<uint64_t, 9> ReadEvents() {
            constexpr auto order = std::memory_order_relaxed;
            return {Events::newTextures.load(order), Events::newTextureBytes.load(order),
                    Events::newBuffers.load(order), Events::newBufferBytes.load(order),
                    Events::bufferWrites.load(order), Events::bufferWriteBytes.load(order),
                    Events::bufferCopyBytes.load(order), Events::sampledNullTextures.load(order),
                    Events::guestRejectedDraws.load(order)};
        }

        static std::array<uint64_t, 21> ReadDetails() {
            constexpr auto o = std::memory_order_relaxed;
            return {Events::fallbackPool.load(o), Events::fallbackFormat.load(o),
                    Events::fallbackUnmapped.load(o), Events::fallbackInvalid.load(o),
                    Events::nullBindlessSampled.load(o), Events::nullBindlessStorage.load(o),
                    Events::tscOutOfRange.load(o),
                    Events::samplersCreated.load(o), Events::samplerMipDisabled.load(o),
                    Events::samplerClampZero.load(o), Events::samplerMinPositive.load(o),
                    Events::samplerBiasNonzero.load(o), Events::samplerAnisotropic.load(o),
                    Events::guestToHostUploads.load(o), Events::guestToHostBytes.load(o),
                    Events::hostToGuestReadbacks.load(o), Events::hostToGuestBytes.load(o),
                    Events::ticBuilt.load(o), Events::ticBaseMipNonzero.load(o),
                    Events::mipGrowths.load(o), Events::mipLevelsAdded.load(o)};
        }

      public:
        void Record(const DrawObservation &a) {
            ++window.draws;
            if (a.path == 1) ++window.indirect;
            else if (a.path == 2) ++window.inlineIndex;
            else if (a.path == 3) ++window.drawTexture;
            else ++window.direct;
            if (a.indexed) ++window.indexed;
            window.elements += a.elements * a.instances;
            if (!a.elements || !a.instances) ++window.empty;
            if (!a.hasColor) ++window.noColor;
            if (!a.hasDepth) ++window.noDepth;
            if (a.zeroScissor) ++window.scissorZero;
            if (a.depthTest) ++window.depthTest;
            if (a.depthWrite) ++window.depthWrite;
            if (a.alphaTest) ++window.alphaTest;
            if (a.rasterDiscard) ++window.rasterDiscard;
            if (a.cull) ++window.cull;
            if (a.predicate) ++window.predicate;
            window.textureSlots += a.textureSlots;
            window.nullSlots += a.nullSlots;
            window.baseMip += a.baseMipNonzero;
            window.partialMip += a.partialMipViews;
            window.mip0 += a.mip0;
            window.mip1 += a.mip1;
            window.mip2 += a.mip2;
            window.mip3Plus += a.mip3Plus;
            window.singleMip += a.singleMip;
            window.mipOutOfRange += a.mipOutOfRange;
            if (a.path == 3) return; // Fixed-function textured rectangle.
            // Track 32 exact shader+texture/mip signatures. A pointer-based texture
            // fingerprint is only comparable inside one process/session.
            bool materialFound{};
            for (auto &s : window.materials) {
                if (s.uses && s.key == a.materialKey) {
                    ++s.uses;
                    materialFound = true;
                    break;
                }
            }
            if (!materialFound) {
                for (auto &s : window.materials) {
                    if (!s.uses) {
                        s = {a.materialKey, 1};
                        materialFound = true;
                        break;
                    }
                }
                if (!materialFound) ++window.extraMaterials;
            }
            for (auto &s : window.signatures)
                if (s.uses && s.key == a.shaderKey) { ++s.uses; return; }
            for (auto &s : window.signatures)
                if (!s.uses) { s = {a.shaderKey, 1}; return; }
            ++window.extraSignatures; // Bounded inventory, not full shader coverage.
        }

        // Per-executor submission interval, not per-game-frame. No GPU sync.
        void Report(uint64_t submission) {
            const uint64_t interval = reportSerial ? 96 : 16;
            if (!window.draws || submission < lastSubmission || submission - lastSubmission < interval)
                return;
            const auto current = ReadEvents();
            std::array<uint64_t, 9> delta{};
            for (size_t i = 0; i < delta.size(); ++i)
                delta[i] = current[i] - previous[i];
            previous = current;
            const auto details = ReadDetails();
            std::array<uint64_t, 21> extra{};
            for (size_t i = 0; i < extra.size(); ++i)
                extra[i] = details[i] - previousDetails[i];
            previousDetails = details;

            std::array<Signature, 4> leaders{};
            std::array<Signature, 3> materialLeaders{};
            for (auto s : window.signatures) {
                for (size_t i = 0; i < leaders.size(); ++i) {
                    if (s.uses > leaders[i].uses) {
                        for (size_t j = leaders.size() - 1; j > i; --j)
                            leaders[j] = leaders[j - 1];
                        leaders[i] = s;
                        break;
                    }
                }
            }
            for (auto entry : window.materials) {
                for (size_t i = 0; i < materialLeaders.size(); ++i) {
                    if (entry.uses > materialLeaders[i].uses) {
                        for (size_t j = materialLeaders.size() - 1; j > i; --j)
                            materialLeaders[j] = materialLeaders[j - 1];
                        materialLeaders[i] = entry;
                        break;
                    }
                }
            }
            ++reportSerial;
            LOGI("[OCEAN-GEO] seq={} submit={} total={} direct={} indexed={} indirect={} inline={} drawTexture={} elements={} empty={} scissorZero={}",
                 reportSerial, submission, window.draws, window.direct, window.indexed,
                 window.indirect, window.inlineIndex, window.drawTexture, window.elements,
                 window.empty, window.scissorZero);
            LOGI("[OCEAN-STATE] seq={} noColor={} noDepth={} depthTest={} depthWrite={} alphaTest={} cull={} rasterDiscard={} predicated={} texSlots={} nullSlots={} mipBase={} partialMip={}",
                 reportSerial, window.noColor, window.noDepth, window.depthTest,
                 window.depthWrite, window.alphaTest, window.cull, window.rasterDiscard,
                 window.predicate, window.textureSlots, window.nullSlots,
                 window.baseMip, window.partialMip);
            LOGI("[OCEAN-SHADERS] seq={} shader0=0x{:016X}:{} shader1=0x{:016X}:{} shader2=0x{:016X}:{} shader3=0x{:016X}:{} untracked={}",
                 reportSerial, leaders[0].key, leaders[0].uses, leaders[1].key, leaders[1].uses,
                 leaders[2].key, leaders[2].uses, leaders[3].key, leaders[3].uses, window.extraSignatures);
            LOGI("[OCEAN-STREAM] seq={} texturesNew={} textureBytes={} buffersNew={} bufferBytes={} bufferWriteCalls={} bufferWriteBytes={} bufferCopyBytes={} nullTextureFallbacks={} guestRejectedDraws={}",
                 reportSerial, delta[0], delta[1], delta[2], delta[3],
                 delta[4], delta[5], delta[6], delta[7], delta[8]);
            LOGI("[OCEAN-FALLBACK] seq={} poolIndex={} format={} unmapped={} invalidTic={} bindlessSampled={} bindlessStorage={} samplerPoolOob={}",
                 reportSerial, extra[0], extra[1], extra[2], extra[3],
                 extra[4], extra[5], extra[6]);
            LOGI("[OCEAN-LOD] seq={} created={} mipNone={} maxLodNear0={} minLodPositive={} biasNonzero={} anisotropic={}",
                 reportSerial, extra[7], extra[8], extra[9], extra[10], extra[11], extra[12]);
            LOGI("[OCEAN-SYNC] seq={} guestUploads={} uploadBytes={} guestReadbacks={} readbackBytes={} ticBuilt={} baseMipNonzero={} mipGrowths={} mipLevelsAdded={}",
                 reportSerial, extra[13], extra[14], extra[15], extra[16], extra[17], extra[18],
                 extra[19], extra[20]);
            LOGI("[OCEAN-MIPS] seq={} mip0={} mip1={} mip2={} mip3Plus={} singleLevelViews={} outOfRangeViews={}",
                 reportSerial, window.mip0, window.mip1, window.mip2,
                 window.mip3Plus, window.singleMip, window.mipOutOfRange);
            LOGI("[OCEAN-MATERIAL] seq={} material0=0x{:016X}:{} material1=0x{:016X}:{} material2=0x{:016X}:{} untracked={}",
                 reportSerial, materialLeaders[0].key, materialLeaders[0].uses,
                 materialLeaders[1].key, materialLeaders[1].uses,
                 materialLeaders[2].key, materialLeaders[2].uses, window.extraMaterials);
            window = {};
            lastSubmission = submission;
        }
    };
}
