// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <fstream>
#include <string_view>
#include <range/v3/algorithm.hpp>
#include <boost/functional/hash.hpp>
#include <gpu.h>
#include <common/settings.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/common/log.h>
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
        bool GraphicsDynamicTextureHandlesSupported(const TraitManager &traits, Shader::Stage stage) {
            if (!traits.supportsDynamicTextureHandles) return false;
            switch (stage) {
                case Shader::Stage::Fragment:
                    return traits.supportsFragmentStoresAndAtomics;
                case Shader::Stage::VertexA:
                case Shader::Stage::VertexB:
                case Shader::Stage::TessellationControl:
                case Shader::Stage::TessellationEval:
                case Shader::Stage::Geometry:
                    return traits.supportsVertexPipelineStoresAndAtomics;
                default:
                    return false;
            }
        }

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

        // Targeted MK11 menu compute shader. Never enable DumpShaders globally.
        constexpr u64 Mk11TargetComputeHash{0x55736CD732068324ULL};
        constexpr size_t Mk11TraceMaxBytes{256 * 1024};
        constexpr size_t Mk11TraceChunkBytes{256};

        // Hex chunks are independently recoverable from emulation.log (including
        // on Android, where app-private shader dump directories are hard to reach).
        void TraceShaderPayload(std::string_view label, u64 hash, std::string_view phase, const u8 *data, size_t size) {
            constexpr char HexDigits[]{"0123456789ABCDEF"};
            const size_t captured{std::min(size, Mk11TraceMaxBytes)};
            LOGW("{}DumpBegin: hash=0x{:016X} phase={} totalBytes={} capturedBytes={} truncated={}",
                 label, hash, phase, size, captured, size > captured);
            for (size_t offset{}; offset < captured; offset += Mk11TraceChunkBytes) {
                const size_t count{std::min(Mk11TraceChunkBytes, captured - offset)};
                std::string hex(count * 2, '0');
                for (size_t i{}; i < count; ++i) {
                    const u8 byte{data[offset + i]};
                    hex[2 * i] = HexDigits[byte >> 4];
                    hex[2 * i + 1] = HexDigits[byte & 0xF];
                }
                LOGW("{}DumpChunk: phase={} offset={} hex={}", label, phase, offset, hex);
            }
            LOGW("{}DumpEnd: hash=0x{:016X} phase={} capturedBytes={}",
                 label, hash, phase, captured);
        }

        void TraceMk11ShaderPayload(std::string_view phase, const u8 *data, size_t size) {
            TraceShaderPayload("MK11Shader", Mk11TargetComputeHash, phase, data, size);
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

        constexpr u32 TegraX1WarpSize{32}; //!< The amount of threads in a warp on the Tegra X1
        profile = Shader::Profile{
            .supported_spirv = traits.supportsSpirv14 ? 0x00010400U : 0x00010000U,
            .unified_descriptor_binding = true,
            .support_descriptor_aliasing = true,
            .support_dynamic_texture_handles = traits.supportsDynamicTextureHandles,
            .support_min_lod = traits.supportsShaderResourceMinLod,
            .support_int8 = traits.supportsInt8,
            .support_int16 = traits.supportsInt16,
            .support_storage8 = traits.supportsStorage8,
            .support_storage16 = traits.supportsStorage16,
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
            .support_viewport_index_layer_non_geometry = traits.supportsShaderViewportIndexLayer,
            .support_viewport_mask = false,
            .support_typeless_image_loads = traits.supportsImageReadWithoutFormat,
            .support_typeless_image_writes = traits.supportsShaderStorageImageWriteWithoutFormat,
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
        bool supportsDynamicTextureHandles;
        ShaderManager::ConstantBufferRead constantBufferRead;
        ShaderManager::GetTextureType getTextureType;
        ShaderManager::GetTextureComponentType getTextureComponentType;
        ShaderManager::GetTextureCompareFunc getTextureCompareFunc;
      
      public:
        GraphicsEnvironment(const std::array<u32, 8> &postVtgShaderAttributeSkipMask,
                            Shader::Stage pStage,
                            span<u8> pBinary, u32 baseOffset,
                            u32 textureBufferIndex,
                            bool viewportTransformEnabled,
                            ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType,
                            ShaderManager::GetTextureComponentType getTextureComponentType,
                            ShaderManager::GetTextureCompareFunc getTextureCompareFunc, bool dynamicTextureHandles)
            : binary{pBinary}, baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              viewportTransformEnabled{viewportTransformEnabled},
              supportsDynamicTextureHandles{dynamicTextureHandles},
              constantBufferRead{std::move(constantBufferRead)}, getTextureType{std::move(getTextureType)},
              getTextureComponentType{std::move(getTextureComponentType)},
              getTextureCompareFunc{std::move(getTextureCompareFunc)} {
            gp_passthrough_mask = postVtgShaderAttributeSkipMask;
            stage = pStage;
            sph = *reinterpret_cast<Shader::ProgramHeader *>(binary.data());
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] bool SupportsDynamicTextureHandles() const final {
            return supportsDynamicTextureHandles;
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
            return getTextureComponentType(handle) != Shader::TextureComponentType::Float;
        }

        [[nodiscard]] Shader::TextureComponentType ReadTextureComponentType(u32 handle) final {
            return getTextureComponentType(handle);
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
        ShaderManager::GetTextureComponentType getTextureComponentType;
        ShaderManager::GetTextureCompareFunc getTextureCompareFunc;
        bool diagnosticTexturePass{};
        bool supportsDynamicTextureHandles{};
    
      public:
        ComputeEnvironment(span<u8> pBinary,
                           u32 baseOffset,
                           u32 textureBufferIndex,
                           u32 localMemorySize, u32 sharedMemorySize,
                           std::array<u32, 3> workgroupDimensions,
                           ShaderManager::ConstantBufferRead constantBufferRead, ShaderManager::GetTextureType getTextureType,
                           ShaderManager::GetTextureComponentType getTextureComponentType,
                           ShaderManager::GetTextureCompareFunc getTextureCompareFunc,
                           bool traceTexturePass, bool dynamicTextureHandles)
            : binary{pBinary},
              baseOffset{baseOffset},
              textureBufferIndex{textureBufferIndex},
              localMemorySize{localMemorySize},
              sharedMemorySize{sharedMemorySize},
              workgroupDimensions{workgroupDimensions},
              constantBufferRead{std::move(constantBufferRead)},
              getTextureType{std::move(getTextureType)},
              getTextureComponentType{std::move(getTextureComponentType)},
              getTextureCompareFunc{std::move(getTextureCompareFunc)},
              diagnosticTexturePass{traceTexturePass},
              supportsDynamicTextureHandles{dynamicTextureHandles} {
            stage = Shader::Stage::Compute;
            start_address = baseOffset;
            is_propietary_driver = textureBufferIndex == 2;
        }

        [[nodiscard]] bool SupportsDynamicTextureHandles() const final {
            return supportsDynamicTextureHandles;
        }

        [[nodiscard]] bool WantsTexturePassDiagnostic() const final {
            return diagnosticTexturePass;
        }

        void TraceTexturePassDiagnostic(std::string_view phase, std::string_view ir) final {
            TraceMk11ShaderPayload(phase, reinterpret_cast<const u8 *>(ir.data()), ir.size());
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
            return getTextureComponentType(handle) != Shader::TextureComponentType::Float;
        }

        [[nodiscard]] Shader::TextureComponentType ReadTextureComponentType(u32 handle) final {
            return getTextureComponentType(handle);
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
                                                           const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType, const GetTextureComponentType &getTextureComponentType, const GetTextureCompareFunc &getTextureCompareFunc) {
        binary = ProcessShaderBinary(false, hash, binary);

        std::scoped_lock lock{poolMutex};

        GraphicsEnvironment environment{postVtgShaderAttributeSkipMask, stage, binary, baseOffset, textureConstantBufferIndex, viewportTransformEnabled, constantBufferRead, getTextureType, getTextureComponentType, getTextureCompareFunc,
            GraphicsDynamicTextureHandlesSupported(gpu.traits, stage)};
        try {
            Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset + sizeof(Shader::ProgramHeader))}};
            return Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
        } catch (const std::exception &error) {
            LOGE("ShaderTranslationFailure: stage={} hash=0x{:016X} baseOffset=0x{:X} dynamicHandles={} reason={}",
                 ShaderStageName(stage), hash, baseOffset, environment.SupportsDynamicTextureHandles(), error.what());
            try {
                // Failure-only capture: successful shaders incur no copy or log.
                TraceShaderPayload("ShaderFailure", hash, "maxwell_translation_input", binary.data(), binary.size());
            } catch (...) {
                // Preserve the original exception if diagnostic allocation fails.
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
                                                          const ConstantBufferRead &constantBufferRead, const GetTextureType &getTextureType, const GetTextureComponentType &getTextureComponentType, const GetTextureCompareFunc &getTextureCompareFunc) {
        const auto guestMaxwellBinary{binary};
        binary = ProcessShaderBinary(false, hash, binary);

        std::scoped_lock lock{poolMutex};

        // One targeted capture, including the *original* Maxwell binary and
        // compiler IR before and after texture handle lowering. No guest data
        // or pipeline behavior is modified by diagnostic output.
        const bool traceTarget{hash == Mk11TargetComputeHash && !targetComputeShaderCaptured};
        if (traceTarget) {
            targetComputeShaderCaptured = true;
            try {
                const bool replaced{binary.data() != guestMaxwellBinary.data() || binary.size() != guestMaxwellBinary.size()};
                LOGW("MK11ShaderTrace: hash=0x{:016X} baseOffset=0x{:X} guestBinaryBytes={} translationBinaryBytes={} replaced={} textureCbuf={}",
                     hash, baseOffset, guestMaxwellBinary.size(), binary.size(), replaced, textureConstantBufferIndex);
                TraceMk11ShaderPayload("maxwell", guestMaxwellBinary.data(), guestMaxwellBinary.size());
                if (replaced)
                    TraceMk11ShaderPayload("maxwell_translation_input", binary.data(), binary.size());
            } catch (...) {
                // Failure to record the trace must not affect guest execution.
            }
        }

        ComputeEnvironment environment{binary, baseOffset, textureConstantBufferIndex, localMemorySize, sharedMemorySize, workgroupDimensions, constantBufferRead, getTextureType, getTextureComponentType, getTextureCompareFunc, traceTarget, gpu.traits.supportsDynamicTextureHandles};
        try {
            Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset)}};
            return Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
        } catch (const std::exception &error) {
            LOGE("ShaderTranslationFailure: stage=Compute hash=0x{:016X} baseOffset=0x{:X} dynamicHandles={} reason={}",
                 hash, baseOffset, environment.SupportsDynamicTextureHandles(), error.what());
            throw;
        }
    }

    vk::ShaderModule ShaderManager::CompileShader(const Shader::RuntimeInfo &runtimeInfo, Shader::IR::Program &program, Shader::Backend::Bindings &bindings, u64 hash) {
        std::scoped_lock lock{poolMutex};

        if (program.info.loads.Legacy() || program.info.stores.Legacy())
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtimeInfo);

        auto spirvEmitted{[&] {
            try {
                return Shader::Backend::SPIRV::EmitSPIRV(profile, runtimeInfo, program, bindings);
            } catch (const std::exception &error) {
                LOGE("ShaderEmissionFailure: stage={} hash=0x{:016X} reason={}", ShaderStageName(program.stage), hash, error.what());
                throw;
            }
        }()};
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
