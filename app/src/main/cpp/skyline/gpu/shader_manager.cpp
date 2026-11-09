// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <range/v3/algorithm.hpp>
#include <boost/functional/hash.hpp>
#include <gpu.h>
#include <common/settings.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/exception.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/backend/spirv/emit_spirv.h>
#include <vulkan/vulkan_raii.hpp>
#include "shader_manager.h"

static constexpr bool DumpShaders{false};

namespace Shader::Log {
    void Debug(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Debug, std::move(const_cast<std::string &>(message)));
    }

    void Warn(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Warning, std::move(const_cast<std::string &>(message)));
    }

    void Error(const std::string &message) {
        skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Error, std::move(const_cast<std::string &>(message)));
    }
}

namespace skyline::gpu {
    namespace {
        const char *ShaderStageName(Shader::Stage stage) {
            switch (stage) {
                case Shader::Stage::VertexA: return "VertexA";
                case Shader::Stage::VertexB: return "VertexB";
                case Shader::Stage::TessellationControl: return "TessellationControl";
                case Shader::Stage::TessellationEval: return "TessellationEval";
                case Shader::Stage::Geometry: return "Geometry";
                case Shader::Stage::Fragment: return "Fragment";
                case Shader::Stage::Compute: return "Compute";
                default: return "Unknown";
            }
        }

        vk::ShaderStageFlagBits VulkanShaderStage(Shader::Stage stage) {
            switch (stage) {
                case Shader::Stage::VertexA:
                case Shader::Stage::VertexB:
                    return vk::ShaderStageFlagBits::eVertex;
                case Shader::Stage::TessellationControl:
                    return vk::ShaderStageFlagBits::eTessellationControl;
                case Shader::Stage::TessellationEval:
                    return vk::ShaderStageFlagBits::eTessellationEvaluation;
                case Shader::Stage::Geometry:
                    return vk::ShaderStageFlagBits::eGeometry;
                case Shader::Stage::Fragment:
                    return vk::ShaderStageFlagBits::eFragment;
                case Shader::Stage::Compute:
                    return vk::ShaderStageFlagBits::eCompute;
                default:
                    return vk::ShaderStageFlagBits::eAll;
            }
        }

        struct SubgroupDiagnosticCounts {
            u32 laneId{};
            u32 voteAll{};
            u32 voteAny{};
            u32 voteEqual{};
            u32 ballot{};
            u32 eqMask{};
            u32 ltMask{};
            u32 leMask{};
            u32 gtMask{};
            u32 geMask{};
            u32 shuffleIndex{};
            u32 shuffleUp{};
            u32 shuffleDown{};
            u32 shuffleButterfly{};
            u32 fswzadd{};

            u32 shuffleIndexImmediate{};
            u32 shuffleIndexDynamic{};
            u32 shuffleClampImmediate{};
            u32 shuffleClampDynamic{};
            u32 shuffleSegmentationImmediate{};
            u32 shuffleSegmentationDynamic{};

            [[nodiscard]] bool Any() const {
                return laneId || voteAll || voteAny || voteEqual || ballot ||
                       eqMask || ltMask || leMask || gtMask || geMask ||
                       shuffleIndex || shuffleUp || shuffleDown || shuffleButterfly || fswzadd;
            }
        };

        void CountShuffleArguments(const Shader::IR::Inst &inst, SubgroupDiagnosticCounts &counts) {
            const auto countArg = [](const Shader::IR::Value &arg, u32 &immediate, u32 &dynamic) {
                if (arg.IsImmediate())
                    ++immediate;
                else
                    ++dynamic;
            };
            countArg(inst.Arg(1), counts.shuffleIndexImmediate, counts.shuffleIndexDynamic);
            countArg(inst.Arg(2), counts.shuffleClampImmediate, counts.shuffleClampDynamic);
            countArg(inst.Arg(3), counts.shuffleSegmentationImmediate, counts.shuffleSegmentationDynamic);
        }

        SubgroupDiagnosticCounts CountSubgroupOperations(const Shader::IR::Program &program) {
            SubgroupDiagnosticCounts counts{};
            for (const auto *block : program.blocks) {
                for (const auto &inst : block->Instructions()) {
                    switch (inst.GetOpcode()) {
                        case Shader::IR::Opcode::LaneId:
                            ++counts.laneId;
                            break;
                        case Shader::IR::Opcode::VoteAll:
                            ++counts.voteAll;
                            break;
                        case Shader::IR::Opcode::VoteAny:
                            ++counts.voteAny;
                            break;
                        case Shader::IR::Opcode::VoteEqual:
                            ++counts.voteEqual;
                            break;
                        case Shader::IR::Opcode::SubgroupBallot:
                            ++counts.ballot;
                            break;
                        case Shader::IR::Opcode::SubgroupEqMask:
                            ++counts.eqMask;
                            break;
                        case Shader::IR::Opcode::SubgroupLtMask:
                            ++counts.ltMask;
                            break;
                        case Shader::IR::Opcode::SubgroupLeMask:
                            ++counts.leMask;
                            break;
                        case Shader::IR::Opcode::SubgroupGtMask:
                            ++counts.gtMask;
                            break;
                        case Shader::IR::Opcode::SubgroupGeMask:
                            ++counts.geMask;
                            break;
                        case Shader::IR::Opcode::ShuffleIndex:
                            ++counts.shuffleIndex;
                            CountShuffleArguments(inst, counts);
                            break;
                        case Shader::IR::Opcode::ShuffleUp:
                            ++counts.shuffleUp;
                            CountShuffleArguments(inst, counts);
                            break;
                        case Shader::IR::Opcode::ShuffleDown:
                            ++counts.shuffleDown;
                            CountShuffleArguments(inst, counts);
                            break;
                        case Shader::IR::Opcode::ShuffleButterfly:
                            ++counts.shuffleButterfly;
                            CountShuffleArguments(inst, counts);
                            break;
                        case Shader::IR::Opcode::FSwizzleAdd:
                            ++counts.fswzadd;
                            break;
                        default:
                            break;
                    }
                }
            }
            return counts;
        }

        void LogUnsupportedSubgroupShader(const Shader::IR::Program &program, u64 hash, u32 guestStartAddress,
                                          const TraitManager &traits, const Shader::Profile &profile) {
            const auto hostStage{VulkanShaderStage(program.stage)};
            if (static_cast<bool>(traits.subgroupSupportedStages & hostStage))
                return;

            const auto counts{CountSubgroupOperations(program)};
            if (!counts.Any())
                return;

            // Parsing is serialized by ShaderManager::poolMutex, so this diagnostic set
            // needs no separate lock. Keep the log bounded and one-shot per shader/stage.
            static std::unordered_set<u64> loggedShaders;
            constexpr size_t MaxLoggedShaders{64};
            const u64 key{hash ^ (static_cast<u64>(program.stage) << 56)};
            if (loggedShaders.find(key) != loggedShaders.end() || loggedShaders.size() >= MaxLoggedShaders)
                return;
            loggedShaders.insert(key);

            LOGW("SubgroupUnsupported: hash=0x{:016X} stage={} guestStart=0x{:X} "
                 "hostStages={} hostStagesRaw=0x{:X} compilerMask=0x{:X} subgroupSize={} spirv=0x{:X} "
                 "supportVote={} warpLarger={} brokenShuffle={} disableShuffle={} "
                 "info={{vote={},invocationId={},shuffles={},mask={},fswzadd={}}} "
                 "ops={{laneId={},voteAll={},voteAny={},voteEqual={},ballot={},eqMask={},ltMask={},leMask={},gtMask={},geMask={},"
                 "shuffleIndex={},shuffleUp={},shuffleDown={},shuffleButterfly={},fswzadd={}}} "
                 "shuffleArgs={{indexImm={},indexDyn={},clampImm={},clampDyn={},segImm={},segDyn={}}} "
                 "wouldFallback={{laneId=0,voteAll=pred,voteAny=pred,voteEqual=true,ballot=pred?1:0,"
                 "eqMask=1,ltMask=0,leMask=1,gtMask=0,geMask=1,shuffle=value+inBounds:false,fswzadd=lane0-safe}}",
                 hash, ShaderStageName(program.stage), guestStartAddress,
                 vk::to_string(traits.subgroupSupportedStages),
                 static_cast<VkShaderStageFlags>(traits.subgroupSupportedStages),
                 profile.supported_subgroup_stages, traits.subgroupSize, profile.supported_spirv,
                 profile.support_vote, profile.warp_size_potentially_larger_than_guest,
                 profile.has_broken_spirv_subgroup_shuffle, profile.disable_subgroup_shuffle,
                 program.info.uses_subgroup_vote, program.info.uses_subgroup_invocation_id,
                 program.info.uses_subgroup_shuffles, program.info.uses_subgroup_mask,
                 program.info.uses_fswzadd,
                 counts.laneId, counts.voteAll, counts.voteAny, counts.voteEqual, counts.ballot,
                 counts.eqMask, counts.ltMask, counts.leMask, counts.gtMask, counts.geMask,
                 counts.shuffleIndex, counts.shuffleUp, counts.shuffleDown, counts.shuffleButterfly,
                 counts.fswzadd,
                 counts.shuffleIndexImmediate, counts.shuffleIndexDynamic,
                 counts.shuffleClampImmediate, counts.shuffleClampDynamic,
                 counts.shuffleSegmentationImmediate, counts.shuffleSegmentationDynamic);
        }

        // Only this fatal translation path uses synchronous CPU file logging.
        // Successful shaders and the normal logger retain their existing paths.
        void LogBindlessFailure(std::string message) {
            AsyncLogger::LogSync(AsyncLogger::LogLevel::Error, std::move(message));
        }

        void TraceBindlessPayload(u64 hash, std::string_view kind, span<const u8> data) {
            constexpr size_t MaxBytes{2 * 1024 * 1024};
            constexpr size_t ChunkBytes{768};
            constexpr char Hex[]{"0123456789ABCDEF"};
            const size_t captured{std::min(data.size(), MaxBytes)};
            LogBindlessFailure(fmt::format("BindlessTracking{}Begin: hash=0x{:016X} bytes={} captured={} truncated={}",
                                          kind, hash, data.size(), captured, data.size() > captured));
            for (size_t offset{}; offset < captured; offset += ChunkBytes) {
                const size_t count{std::min(ChunkBytes, captured - offset)};
                std::string hex(count * 2, '0');
                for (size_t i{}; i < count; ++i) {
                    const u8 byte{data[offset + i]};
                    hex[2 * i] = Hex[byte >> 4];
                    hex[2 * i + 1] = Hex[byte & 15];
                }
                LogBindlessFailure(fmt::format("BindlessTracking{}Chunk: hash=0x{:016X} offset={} hex={}",
                                              kind, hash, offset, hex));
            }
            LogBindlessFailure(fmt::format("BindlessTracking{}End: hash=0x{:016X} captured={}", kind, hash, captured));
        }

        void TraceBindlessFailure(u64 hash, span<u8> binary, std::string_view diagnostic_ir) {
            TraceBindlessPayload(hash, "Maxwell", {binary.data(), binary.size()});
            if (diagnostic_ir.empty()) {
                LogBindlessFailure(fmt::format("BindlessTrackingIRUnavailable: hash=0x{:016X} reason=no IR snapshot", hash));
                return;
            }
            TraceBindlessPayload(hash, "IR", {reinterpret_cast<const u8*>(diagnostic_ir.data()), diagnostic_ir.size()});
        }

    }

    void ShaderManager::LoadShaderReplacements(std::string_view replacementDir) {
        std::filesystem::path replacementDirPath{replacementDir};
        if (std::filesystem::exists(replacementDirPath)) {
            for (const auto &entry : std::filesystem::directory_iterator{replacementDirPath}) {
                if (entry.is_regular_file()) {
                    // Parse hash from filename
                    auto path{entry.path()};
                    auto &replacementMap{path.extension().string() == ".spv" ? hostShaderReplacements : guestShaderReplacements};
                    u64 hash{std::stoull(path.stem().string(), nullptr, 16)};
                    auto it{replacementMap.insert({hash, {}})};

                    // Read file into map entry
                    std::ifstream file{entry.path(), std::ios::binary | std::ios::ate};
                    it.first->second.resize(static_cast<size_t>(file.tellg()));
                    file.seekg(0, std::ios::beg);
                    file.read(reinterpret_cast<char *>(it.first->second.data()), static_cast<std::streamsize>(it.first->second.size()));
                }
            }
        }
    }

    span<u8> ShaderManager::ProcessShaderBinary(bool spv, u64 hash, span<u8> binary) {
        auto &replacementMap{spv ? hostShaderReplacements : guestShaderReplacements};
        auto it{replacementMap.find(hash)};
        if (it != replacementMap.end()) {
            LOGI("Replacing shader with hash: 0x{:X}", hash);
            return it->second;
        }

        if (DumpShaders) {
            std::scoped_lock lock{dumpMutex};

            auto shaderPath{dumpPath / fmt::format("{:016X}{}", hash, spv ? ".spv" : "")};
            if (!std::filesystem::exists(shaderPath)) {
                std::ofstream file{shaderPath, std::ios::binary};
                file.write(reinterpret_cast<const char *>(binary.data()), static_cast<std::streamsize>(binary.size()));
            }
        }

        return binary;
    }

    ShaderManager::ShaderManager(const DeviceState &state, GPU &gpu, std::string_view replacementDir, std::string_view dumpDir) : gpu{gpu}, dumpPath{dumpDir} {
        LoadShaderReplacements(replacementDir);

        if constexpr (DumpShaders) {
            if (!std::filesystem::exists(dumpPath))
                std::filesystem::create_directories(dumpPath);
        }

        auto &traits{gpu.traits};
        hostTranslateInfo = Shader::HostTranslateInfo{
            .support_float16 = traits.supportsFloat16,
            .support_int64 = traits.supportsInt64,
            .needs_demote_reorder = false,
            .support_snorm_render_buffer = true,
            .support_viewport_index_layer = gpu.traits.supportsShaderViewportIndexLayer,
            .min_ssbo_alignment = traits.minimumStorageBufferAlignment,
            .support_geometry_shader_passthrough = false,
            .has_broken_texture_shadow_compare = traits.quirks.brokenTextureShadowCompare,
        };

        const auto subgroupStageBit{[&](vk::ShaderStageFlagBits flag, Shader::Stage stage) -> u32 {
            return static_cast<bool>(traits.subgroupSupportedStages & flag)
                ? (1u << static_cast<u32>(stage))
                : 0u;
        }};
        const u32 supportedSubgroupStages{
            subgroupStageBit(vk::ShaderStageFlagBits::eVertex, Shader::Stage::VertexA) |
            subgroupStageBit(vk::ShaderStageFlagBits::eVertex, Shader::Stage::VertexB) |
            subgroupStageBit(vk::ShaderStageFlagBits::eTessellationControl, Shader::Stage::TessellationControl) |
            subgroupStageBit(vk::ShaderStageFlagBits::eTessellationEvaluation, Shader::Stage::TessellationEval) |
            subgroupStageBit(vk::ShaderStageFlagBits::eGeometry, Shader::Stage::Geometry) |
            subgroupStageBit(vk::ShaderStageFlagBits::eFragment, Shader::Stage::Fragment) |
            subgroupStageBit(vk::ShaderStageFlagBits::eCompute, Shader::Stage::Compute)
        };
        LOGI("Shader subgroup stages: host={} compilerMask=0x{:X}",
             vk::to_string(traits.subgroupSupportedStages), supportedSubgroupStages);

        constexpr u32 TegraX1WarpSize{32}; //!< The amount of threads in a warp on the Tegra X1
        profile = Shader::Profile{
            .supported_spirv = traits.supportsSpirv14 ? 0x00010400U : 0x00010000U,
            .unified_descriptor_binding = true,
            .support_descriptor_aliasing = true,
            .support_int8 = traits.supportsInt8,
            .support_int16 = traits.supportsInt16,
            .support_int64 = traits.supportsInt64,
            .support_vertex_instance_id = false,
            .support_float_controls = traits.supportsFloatControls,
            .support_separate_denorm_behavior = traits.floatControls.denormBehaviorIndependence == vk::ShaderFloatControlsIndependence::eAll,
            .support_separate_rounding_mode = traits.floatControls.roundingModeIndependence == vk::ShaderFloatControlsIndependence::eAll,
            .support_fp16_denorm_preserve = static_cast<bool>(traits.floatControls.shaderDenormPreserveFloat16),
            .support_fp32_denorm_preserve = static_cast<bool>(traits.floatControls.shaderDenormPreserveFloat32),
            .support_fp16_denorm_flush = static_cast<bool>(traits.floatControls.shaderDenormFlushToZeroFloat16),
            .support_fp32_denorm_flush = static_cast<bool>(traits.floatControls.shaderDenormFlushToZeroFloat32),
            .support_fp16_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat16),
            .support_fp32_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat32),
            .support_fp64_signed_zero_nan_preserve = static_cast<bool>(traits.floatControls.shaderSignedZeroInfNanPreserveFloat64),
            .support_explicit_workgroup_layout = false,
            .support_vote = traits.supportsSubgroupVote,
            .supported_subgroup_stages = supportedSubgroupStages,
            .support_viewport_index_layer_non_geometry = traits.supportsShaderViewportIndexLayer,
            .support_viewport_mask = false,
            .support_typeless_image_loads = traits.supportsImageReadWithoutFormat,
            .support_demote_to_helper_invocation = traits.supportsShaderDemoteToHelper,
            .support_int64_atomics = traits.supportsAtomicInt64,
            .support_derivative_control = true,
            .support_geometry_shader_passthrough = false,
            .support_native_ndc = false,
            .warp_size_potentially_larger_than_guest = TegraX1WarpSize < traits.subgroupSize,
            .lower_left_origin_mode = false,
            .need_declared_frag_colors = false,
            .has_broken_spirv_position_input = traits.quirks.brokenSpirvPositionInput,
            .has_broken_fp16_float_controls = traits.quirks.brokenFp16FloatControls,
            .has_broken_spirv_subgroup_mask_vector_extract_dynamic = traits.quirks.brokenSubgroupMaskExtractDynamic,
            .has_broken_spirv_subgroup_shuffle = traits.quirks.brokenSubgroupShuffle,
            .max_subgroup_size = traits.subgroupSize,
            .has_broken_spirv_vector_access_chain = traits.quirks.brokenSpirvVectorAccessChain,
            .has_broken_texture_shadow_compare = traits.quirks.brokenTextureShadowCompare,
            .disable_subgroup_shuffle = *state.settings->disableSubgroupShuffle,
        };

        Shader::Settings::values = {
            #ifdef NDEBUG
            .renderer_debug = false,
            .disable_shader_loop_safety_checks = false,
            #else
            .renderer_debug = true,
            .disable_shader_loop_safety_checks = true,
            #endif
            .resolution_info = {
                .active = false,
            },
        };
    }

    /**
     * @brief A shader environment for all graphics pipeline stages
     */
    class GraphicsEnvironment : public Shader::Environment {
      private:
        span<u8> binary;
        u32 baseOffset;
        u32 textureBufferIndex;
        bool viewportTransformEnabled;
        ShaderManager::ConstantBufferRead constantBufferRead;
        ShaderManager::GetTextureType getTextureType;
        ShaderManager::IsTexturePixelFormatInteger isTexturePixelFormatInteger;
        ShaderManager::GetTextureCompareFunc getTextureCompareFunc;
      
      public:
        GraphicsEnvironment(const std::array<u32, 8> &postVtgShaderAttributeSkipMask,
                            Shader::Stage pStage,
                            span<u8> pBinary, u32 baseOffset,
                            u32 textureBufferIndex,
                            bool viewportTransformEnabled,
                            ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType,
                            ShaderManager::IsTexturePixelFormatInteger isTexturePixelFormatInteger,
                            ShaderManager::GetTextureCompareFunc getTextureCompareFunc)
            : binary{pBinary}, baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              viewportTransformEnabled{viewportTransformEnabled},
              constantBufferRead{std::move(constantBufferRead)}, getTextureType{std::move(getTextureType)},
              isTexturePixelFormatInteger{std::move(isTexturePixelFormatInteger)},
              getTextureCompareFunc{std::move(getTextureCompareFunc)} {
            gp_passthrough_mask = postVtgShaderAttributeSkipMask;
            stage = pStage;
            sph = *reinterpret_cast<Shader::ProgramHeader *>(binary.data());
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            address -= baseOffset;
            if (binary.size() < (address + sizeof(u64)))
                throw exception("Out of bounds instruction read: 0x{:X}", address);
            return *reinterpret_cast<u64 *>(binary.data() + address);
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            return constantBufferRead(index, offset);
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("ReadTexturePixelFormat not implemented");
        }

        [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) final {
            return isTexturePixelFormatInteger(handle);
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            return getTextureType(handle);
        }
       
        [[nodiscard]] Shader::CompareFunction ReadTextureCompareFunc(u32 handle) final {
            return getTextureCompareFunc(handle);
        }
        
        [[nodiscard]] u32 ReadViewportTransformState() final {
            return viewportTransformEnabled ? 1 : 0; // Only relevant for graphics shaders
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            return textureBufferIndex;
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return 0; // Only relevant for compute shaders
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return {0, 0, 0}; // Only relevant for compute shaders
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return stage == Shader::Stage::VertexB || stage == Shader::Stage::VertexA;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            if (bank != 0 || !is_propietary_driver)
                return std::nullopt;

            // Replace constant buffer offsets containing draw parameters with the appropriate shader attribute, required as e.g. in the case of indirect draws the constant buffer contents won't be entirely correct for these specific parameters
            switch (offset) {
                case 0x640:
                    return Shader::ReplaceConstant::BaseVertex;
                case 0x644:
                    return Shader::ReplaceConstant::BaseInstance;
                case 0x648:
                    return Shader::ReplaceConstant::DrawID;
                default:
                    return std::nullopt;
            }
        }

        void Dump(u64 hash) final {}
    };

    /**
     * @brief A shader environment for all compute pipeline stages
     */
    class ComputeEnvironment : public Shader::Environment {
      private:
        span<u8> binary;
        u32 baseOffset;
        u32 textureBufferIndex;
        u32 localMemorySize;
        u32 sharedMemorySize;
        std::array<u32, 3> workgroupDimensions;
        ShaderManager::ConstantBufferRead constantBufferRead;
        ShaderManager::GetTextureType getTextureType;
        ShaderManager::IsTexturePixelFormatInteger isTexturePixelFormatInteger;
        ShaderManager::GetTextureCompareFunc getTextureCompareFunc;
    
      public:
        ComputeEnvironment(span<u8> pBinary,
                           u32 baseOffset,
                           u32 textureBufferIndex,
                           u32 localMemorySize, u32 sharedMemorySize,
                           std::array<u32, 3> workgroupDimensions,
                           ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType,
                           ShaderManager::IsTexturePixelFormatInteger isTexturePixelFormatInteger,
                           ShaderManager::GetTextureCompareFunc getTextureCompareFunc)
            : binary{pBinary},
              baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              localMemorySize{localMemorySize},
              sharedMemorySize{sharedMemorySize},
              workgroupDimensions{workgroupDimensions},
              constantBufferRead{std::move(constantBufferRead)},
              getTextureType{std::move(getTextureType)},
              isTexturePixelFormatInteger{std::move(isTexturePixelFormatInteger)},
              getTextureCompareFunc{std::move(getTextureCompareFunc)} {
            stage = Shader::Stage::Compute;
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            address -= baseOffset;
            if (binary.size() < (address + sizeof(u64)))
                throw exception("Out of bounds instruction read: 0x{:X}", address);
            return *reinterpret_cast<u64 *>(binary.data() + address);
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            return constantBufferRead(index, offset);
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("ReadTexturePixelFormat not implemented");
        }

        [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) final {
            return isTexturePixelFormatInteger(handle);
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            return getTextureType(handle);
        }
        
        [[nodiscard]] Shader::CompareFunction ReadTextureCompareFunc(u32 handle) final {
            return getTextureCompareFunc(handle);
        }
        
        [[nodiscard]] u32 ReadViewportTransformState() final {
            return 0; // Only relevant for graphics shaders
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            return textureBufferIndex;
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return localMemorySize;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return sharedMemorySize;
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return workgroupDimensions;
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return false;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            return std::nullopt;
        }

        void Dump(u64 hash) final {}
    };


    /**
     * @brief A shader environment for VertexB during combination as it only requires the shader header and no higher level context
     */
    class VertexBEnvironment : public Shader::Environment {
      public:
        explicit VertexBEnvironment(span<u8> binary) {
            sph = *reinterpret_cast<Shader::ProgramHeader *>(binary.data());
            stage = Shader::Stage::VertexB;
        }

        [[nodiscard]] u64 ReadInstruction(u32 address) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 ReadCbufValue(u32 index, u32 offset) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] Shader::CompareFunction ReadTextureCompareFunc(u32 handle) final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 ReadViewportTransformState() final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 TextureBoundBuffer() const final {
            throw exception("Not implemented");
        }

        [[nodiscard]] u32 LocalMemorySize() const final {
            return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }

        [[nodiscard]] u32 SharedMemorySize() const final {
            return 0; // Only relevant for compute shaders
        }

        [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final {
            return {0, 0, 0}; // Only relevant for compute shaders
        }

        [[nodiscard]] bool HasHLEMacroState() const final {
            return false;
        }

        [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) final {
            return std::nullopt;
        }

        void Dump(u64 hash) final {}
    };

    Shader::IR::Program ShaderManager::ParseGraphicsShader(const std::array<u32, 8> &postVtgShaderAttributeSkipMask,
                                                           Shader::Stage stage,
                                                           u64 hash, span<u8> binary, u32 baseOffset,
                                                           u32 textureConstantBufferIndex,
                                                           bool viewportTransformEnabled,
                                                           const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType, const IsTexturePixelFormatInteger &isTexturePixelFormatInteger, const GetTextureCompareFunc &getTextureCompareFunc) {
        binary = ProcessShaderBinary(false, hash, binary);

        std::scoped_lock lock{poolMutex};

        GraphicsEnvironment environment{postVtgShaderAttributeSkipMask, stage, binary, baseOffset, textureConstantBufferIndex, viewportTransformEnabled, constantBufferRead, getTextureType, isTexturePixelFormatInteger, getTextureCompareFunc};
        try {
            Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset + sizeof(Shader::ProgramHeader))}};
            auto program{Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo)};
            LogUnsupportedSubgroupShader(program, hash, baseOffset, gpu.traits, profile);
            return program;
        } catch (const Shader::BindlessTextureTrackingError &error) {
            try {
                LogBindlessFailure(fmt::format("BindlessTrackingFailure: stage={} hash=0x{:016X} baseOffset=0x{:X} textureCbuf={} viewportTransform={} reason={}",
                     ShaderStageName(stage), hash, baseOffset, textureConstantBufferIndex, viewportTransformEnabled, error.what()));
                LogBindlessFailure(fmt::format("BindlessTrackingGraphicsState: postVtgSkipMask={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
                     postVtgShaderAttributeSkipMask[0], postVtgShaderAttributeSkipMask[1], postVtgShaderAttributeSkipMask[2], postVtgShaderAttributeSkipMask[3],
                     postVtgShaderAttributeSkipMask[4], postVtgShaderAttributeSkipMask[5], postVtgShaderAttributeSkipMask[6], postVtgShaderAttributeSkipMask[7]));
                TraceBindlessFailure(hash, binary, error.DiagnosticIR());
            } catch (...) {
                // Preserve the compiler failure if diagnostic allocation/logging fails.
            }
            throw;
        }
    }

    Shader::IR::Program ShaderManager::CombineVertexShaders(Shader::IR::Program &vertexA, Shader::IR::Program &vertexB, span<u8> vertexBBinary) {
        std::scoped_lock lock{poolMutex};

        VertexBEnvironment env{vertexBBinary};
        return Shader::Maxwell::MergeDualVertexPrograms(vertexA, vertexB, env);
    }

    Shader::IR::Program ShaderManager::GenerateGeometryPassthroughShader(Shader::IR::Program &layerSource, Shader::OutputTopology topology) {
        std::scoped_lock lock{poolMutex};

        return Shader::Maxwell::GenerateGeometryPassthrough(instructionPool, blockPool, hostTranslateInfo, layerSource, topology);
    }

    Shader::IR::Program ShaderManager::ParseComputeShader(u64 hash, span<u8> binary, u32 baseOffset,
                                                          u32 textureConstantBufferIndex,
                                                          u32 localMemorySize, u32 sharedMemorySize,
                                                          std::array<u32, 3> workgroupDimensions,
                                                          const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType, const IsTexturePixelFormatInteger &isTexturePixelFormatInteger, const GetTextureCompareFunc &getTextureCompareFunc) {
        binary = ProcessShaderBinary(false, hash, binary);

        std::scoped_lock lock{poolMutex};

        ComputeEnvironment environment{binary, baseOffset, textureConstantBufferIndex, localMemorySize, sharedMemorySize, workgroupDimensions, constantBufferRead, getTextureType, isTexturePixelFormatInteger, getTextureCompareFunc};
        try {
            Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset)}};
            auto program{Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo)};
            LogUnsupportedSubgroupShader(program, hash, baseOffset, gpu.traits, profile);
            return program;
        } catch (const Shader::BindlessTextureTrackingError &error) {
            try {
                LogBindlessFailure(fmt::format("BindlessTrackingFailure: stage=Compute hash=0x{:016X} baseOffset=0x{:X} textureCbuf={} localMemory={} sharedMemory={} workgroup={},{},{} reason={}",
                     hash, baseOffset, textureConstantBufferIndex, localMemorySize, sharedMemorySize,
                     workgroupDimensions[0], workgroupDimensions[1], workgroupDimensions[2], error.what()));
                TraceBindlessFailure(hash, binary, error.DiagnosticIR());
            } catch (...) {
                // Preserve the compiler failure if diagnostic allocation/logging fails.
            }
            throw;
        }
    }

    vk::ShaderModule ShaderManager::CompileShader(const Shader::RuntimeInfo &runtimeInfo, Shader::IR::Program &program, Shader::Backend::Bindings &bindings, u64 hash) {
        std::scoped_lock lock{poolMutex};

        if (program.info.loads.Legacy() || program.info.stores.Legacy())
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtimeInfo);

        auto spirvEmitted{Shader::Backend::SPIRV::EmitSPIRV(profile, runtimeInfo, program, bindings)};
        auto spirv{ProcessShaderBinary(true, hash, span<u32>{spirvEmitted}.cast<u8>()).cast<u32>()};

        vk::ShaderModuleCreateInfo createInfo{
            .pCode = spirv.data(),
            .codeSize = spirv.size_bytes(),
        };

        return (*gpu.vkDevice).createShaderModule(createInfo, nullptr, *gpu.vkDevice.getDispatcher());
    }

    void ShaderManager::ResetPools() {
        std::scoped_lock lock{poolMutex};

        instructionPool.ReleaseContents();
        blockPool.ReleaseContents();
        flowBlockPool.ReleaseContents();
    }
}
