// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <array>
#include <fstream>
#include <unordered_set>
#include <range/v3/algorithm.hpp>
#include <boost/functional/hash.hpp>
#include <gpu.h>
#include <common/settings.h>
#include <shader_compiler/common/settings.h>
#include <shader_compiler/common/log.h>
#include <shader_compiler/frontend/maxwell/translate_program.h>
#include <shader_compiler/frontend/ir/modifiers.h>
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
    static const char *CbufOpcodeName(Shader::IR::Opcode opcode) {
        switch (opcode) {
            case Shader::IR::Opcode::GetCbufU8: return "u8";
            case Shader::IR::Opcode::GetCbufS8: return "s8";
            case Shader::IR::Opcode::GetCbufU16: return "u16";
            case Shader::IR::Opcode::GetCbufS16: return "s16";
            case Shader::IR::Opcode::GetCbufU32: return "u32";
            case Shader::IR::Opcode::GetCbufF32: return "f32";
            case Shader::IR::Opcode::GetCbufU32x2: return "u32x2";
            default: return nullptr;
        }
    }

    static void TraceCbufDependencies(const Shader::IR::Value &value,
                                      std::unordered_set<const Shader::IR::Inst *> &visited,
                                      std::vector<std::tuple<u32, u32, const char *>> &out,
                                      bool &hasDynamicCbuf) {
        const Shader::IR::Inst *inst{value.TryInstRecursive()};
        if (!inst || !visited.insert(inst).second)
            return;

        if (const char *kind{CbufOpcodeName(inst->GetOpcode())}) {
            const auto index{inst->Arg(0)};
            const auto offset{inst->Arg(1)};
            if (index.IsImmediate() && offset.IsImmediate()) {
                out.emplace_back(index.U32(), offset.U32(), kind);
            } else {
                hasDynamicCbuf = true;
            }
            return;
        }

        for (size_t i{}; i < inst->NumArgs(); i++)
            TraceCbufDependencies(inst->Arg(i), visited, out, hasDynamicCbuf);
    }

    static bool IsNfsLightingVectorOffset(u32 offset) {
        switch (offset) {
            case 0xB0:
            case 0xB4:
            case 0xB8:
            case 0xC0:
            case 0xC4:
            case 0xC8:
                return true;
            default:
                return false;
        }
    }

    static void TraceNfsLightingPaths(const Shader::IR::Value &value,
                                      std::vector<Shader::IR::Opcode> &path,
                                      std::unordered_set<const Shader::IR::Inst *> &active,
                                      std::unordered_set<u32> &loggedOffsets,
                                      u32 component,
                                      u32 depth = 0) {
        if (depth > 64)
            return;

        const Shader::IR::Inst *inst{value.TryInstRecursive()};
        if (!inst || !active.insert(inst).second)
            return;

        path.push_back(inst->GetOpcode());

        if (inst->GetOpcode() == Shader::IR::Opcode::GetCbufF32) {
            const auto index{inst->Arg(0)};
            const auto offset{inst->Arg(1)};
            if (index.IsImmediate() && offset.IsImmediate() && index.U32() == 3 &&
                IsNfsLightingVectorOffset(offset.U32()) && loggedOffsets.insert(offset.U32()).second) {
                std::string chain;
                for (size_t i{}; i < path.size(); i++) {
                    if (i)
                        chain += " <- ";
                    chain += Shader::IR::NameOf(path[i]);
                }
                LOGI("NFS_LIGHT_PATH FS=0xAE29B20939298730 component={} cbuf=3 off=0x{:X} path={}",
                     component, offset.U32(), chain);
            }
        } else {
            for (size_t i{}; i < inst->NumArgs(); i++)
                TraceNfsLightingPaths(inst->Arg(i), path, active, loggedOffsets, component, depth + 1);
        }

        path.pop_back();
        active.erase(inst);
    }

    static bool IsTextureReadOpcode(Shader::IR::Opcode opcode) {
        switch (opcode) {
            case Shader::IR::Opcode::ImageSampleImplicitLod:
            case Shader::IR::Opcode::ImageSampleExplicitLod:
            case Shader::IR::Opcode::ImageSampleDrefImplicitLod:
            case Shader::IR::Opcode::ImageSampleDrefExplicitLod:
            case Shader::IR::Opcode::ImageGather:
            case Shader::IR::Opcode::ImageGatherDref:
            case Shader::IR::Opcode::ImageFetch:
            case Shader::IR::Opcode::ImageQueryLod:
                return true;
            default:
                return false;
        }
    }

    static void TraceNfsTexturePaths(const Shader::IR::Value &value,
                                     const Shader::IR::Program &program,
                                     std::vector<Shader::IR::Opcode> &path,
                                     std::unordered_set<const Shader::IR::Inst *> &active,
                                     std::unordered_set<u64> &logged,
                                     u32 component,
                                     u32 depth = 0) {
        if (depth > 96)
            return;

        const Shader::IR::Inst *inst{value.TryInstRecursive()};
        if (!inst || !active.insert(inst).second)
            return;

        path.push_back(inst->GetOpcode());

        if (IsTextureReadOpcode(inst->GetOpcode())) {
            const auto info{inst->Flags<Shader::IR::TextureInstInfo>()};
            const u32 descriptorIndex{static_cast<u32>(info.descriptor_index)};
            const u64 key{(static_cast<u64>(component) << 32) | descriptorIndex};

            if (logged.insert(key).second) {
                std::string chain;
                for (size_t i{}; i < path.size(); i++) {
                    if (i)
                        chain += " <- ";
                    chain += Shader::IR::NameOf(path[i]);
                }

                if (descriptorIndex < program.info.texture_descriptors.size()) {
                    const auto &desc{program.info.texture_descriptors[descriptorIndex]};
                    LOGI("NFS_TEX_PATH FS=0xAE29B20939298730 component={} desc={} opcode={} type={} depth={} cbuf={} off=0x{:X} count={} secondary={} sec_cbuf={} sec_off=0x{:X} path={}",
                         component, descriptorIndex, Shader::IR::NameOf(inst->GetOpcode()),
                         static_cast<u32>(desc.type), desc.is_depth,
                         desc.cbuf_index, desc.cbuf_offset, desc.count, desc.has_secondary,
                         desc.secondary_cbuf_index, desc.secondary_cbuf_offset, chain);
                } else {
                    LOGI("NFS_TEX_PATH FS=0xAE29B20939298730 component={} desc={} opcode={} flags_type={} flags_depth={} path={}",
                         component, descriptorIndex, Shader::IR::NameOf(inst->GetOpcode()),
                         static_cast<u32>(info.type), static_cast<u32>(info.is_depth), chain);
                }
            }
        } else {
            for (size_t i{}; i < inst->NumArgs(); i++)
                TraceNfsTexturePaths(inst->Arg(i), program, path, active, logged, component, depth + 1);
        }

        path.pop_back();
        active.erase(inst);
    }

    static void LogNfsTextureDependencies(const Shader::IR::Program &program, u64 hash) {
        if (program.stage != Shader::Stage::Fragment || hash != 0xAE29B20939298730ULL)
            return;

        for (const Shader::IR::Block *block : program.blocks) {
            for (const auto &inst : *block) {
                if (inst.GetOpcode() != Shader::IR::Opcode::SetFragColor)
                    continue;

                const auto rt{inst.Arg(0)};
                const auto component{inst.Arg(1)};
                if (!rt.IsImmediate() || !component.IsImmediate() ||
                    rt.U32() != 0 || component.U32() >= 3) {
                    continue;
                }

                std::vector<Shader::IR::Opcode> path{Shader::IR::Opcode::SetFragColor};
                std::unordered_set<const Shader::IR::Inst *> active;
                std::unordered_set<u64> logged;
                TraceNfsTexturePaths(inst.Arg(2), program, path, active, logged, component.U32());
            }
        }
    }

    static void LogNfsLightingOperationPaths(const Shader::IR::Program &program, u64 hash) {
        if (program.stage != Shader::Stage::Fragment || hash != 0xAE29B20939298730ULL)
            return;

        for (const Shader::IR::Block *block : program.blocks) {
            for (const auto &inst : *block) {
                if (inst.GetOpcode() != Shader::IR::Opcode::SetFragColor)
                    continue;

                const auto rt{inst.Arg(0)};
                const auto component{inst.Arg(1)};
                if (!rt.IsImmediate() || !component.IsImmediate() ||
                    rt.U32() != 0 || component.U32() >= 3) {
                    continue;
                }

                std::vector<Shader::IR::Opcode> path{Shader::IR::Opcode::SetFragColor};
                std::unordered_set<const Shader::IR::Inst *> active;
                std::unordered_set<u32> loggedOffsets;
                TraceNfsLightingPaths(inst.Arg(2), path, active, loggedOffsets, component.U32());
            }
        }
    }

    static void LogFragmentCbufSlice(const Shader::IR::Program &program, u64 hash) {
        if (program.stage != Shader::Stage::Fragment || hash != 0xAE29B20939298730ULL)
            return;

        for (const Shader::IR::Block *block : program.blocks) {
            for (const auto &inst : *block) {
                if (inst.GetOpcode() != Shader::IR::Opcode::SetFragColor)
                    continue;

                const auto rt{inst.Arg(0)};
                const auto component{inst.Arg(1)};
                if (!rt.IsImmediate() || !component.IsImmediate() || rt.U32() != 0 || component.U32() >= 3)
                    continue;

                std::unordered_set<const Shader::IR::Inst *> visited;
                std::vector<std::tuple<u32, u32, const char *>> dependencies;
                bool hasDynamicCbuf{};
                TraceCbufDependencies(inst.Arg(2), visited, dependencies, hasDynamicCbuf);

                std::sort(dependencies.begin(), dependencies.end(), [](const auto &lhs, const auto &rhs) {
                    if (std::get<0>(lhs) != std::get<0>(rhs))
                        return std::get<0>(lhs) < std::get<0>(rhs);
                    if (std::get<1>(lhs) != std::get<1>(rhs))
                        return std::get<1>(lhs) < std::get<1>(rhs);
                    return std::string_view{std::get<2>(lhs)} < std::string_view{std::get<2>(rhs)};
                });
                dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

                LOGI("NFS_CBUF_SLICE FS=0x{:016X} rt=0 component={} deps={} dynamic={}",
                     hash, component.U32(), dependencies.size(), hasDynamicCbuf);
                for (const auto &[index, offset, kind] : dependencies) {
                    LOGI("NFS_CBUF_SLICE FS=0x{:016X} rt=0 component={} cbuf={} off=0x{:X} kind={}",
                         hash, component.U32(), index, offset, kind);
                }
            }
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
        Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset + sizeof(Shader::ProgramHeader))}};
        return  Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
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
        Shader::Maxwell::Flow::CFG cfg{environment, flowBlockPool, Shader::Maxwell::Location{static_cast<u32>(baseOffset)}};
        return Shader::Maxwell::TranslateProgram(instructionPool, blockPool, environment, cfg, hostTranslateInfo);
    }

    vk::ShaderModule ShaderManager::CompileShader(const Shader::RuntimeInfo &runtimeInfo, Shader::IR::Program &program, Shader::Backend::Bindings &bindings, u64 hash) {
        std::scoped_lock lock{poolMutex};


        if (program.info.loads.Legacy() || program.info.stores.Legacy())
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtimeInfo);

        LogFragmentCbufSlice(program, hash);
        LogNfsLightingOperationPaths(program, hash);
        LogNfsTextureDependencies(program, hash);

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
