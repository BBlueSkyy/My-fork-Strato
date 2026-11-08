#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Syntax-check the production compute creation boundary against real Vulkan-Hpp.

Non-Vulkan application context is stubbed; no device or Android SDK is needed.
Pass the include directory that contains vulkan/vulkan_raii.hpp.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "app/src/main/cpp/skyline/gpu/interconnect/kepler_compute/pipeline_manager.cpp").read_text()
start = source.index("    static Pipeline::CompiledPipeline MakeCompiledPipeline(")
end = source.index("    Pipeline::Pipeline(", start)
production = source[start:end]
shader_source = (root / "app/src/main/cpp/skyline/gpu/shader_manager.cpp").read_text()
shader_start = shader_source.index("    vk::ShaderModule ShaderManager::CompileShader(")
shader_end = shader_source.index("    void ShaderManager::ResetPools()", shader_start)
shader_production = shader_source[shader_start:shader_end]
stubs = r'''
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <fmt/format.h>
#include <gpu/compute_diagnostics.h>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <shader_compiler/profile.h>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t; using i32=int32_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    template<class Out> span<Out> cast() { return {reinterpret_cast<Out *>(this->data()), this->size_bytes() / sizeof(Out)}; }
};
namespace Shader {
    struct RuntimeInfo {};
    namespace IR { struct Program { struct Info { struct Access { bool Legacy() { return false; } } loads, stores; } info; }; }
    namespace Maxwell { void ConvertLegacyToGeneric(IR::Program &, const RuntimeInfo &) {} }
    namespace Backend {
        struct Bindings {};
        namespace SPIRV { std::vector<u32> EmitSPIRV(const Profile &, const RuntimeInfo &, IR::Program &, Bindings &) { return {}; } }
    }
}
using skyline::gpu::ComputeDiagnosticStore;
constexpr u32 VkApiVersion=VK_API_VERSION_1_1;
struct AsyncLogger {
    enum class LogLevel { Error };
    static void LogSync(LogLevel, std::string &&) {}
};
struct PackedPipelineState {
    u64 shaderHash; std::array<u32,3> dimensions;
    u32 localMemorySize, sharedMemorySize, bindlessTextureConstantBufferSlotSelect;
};
struct ShaderBinary { span<u8> binary; u32 baseOffset; };
struct GPU;
struct ShaderManager {
    GPU &gpu;
    Shader::Profile profile;
    std::mutex poolMutex;
    std::unordered_map<u64, std::vector<u8>> guestShaderReplacements;
    span<u8> ProcessShaderBinary(bool, u64, span<u8> binary) { return binary; }
    vk::ShaderModule CompileShader(const Shader::RuntimeInfo &, Shader::IR::Program &, Shader::Backend::Bindings &, u64,
        const std::function<void(span<const u32>)> &);
    ComputeDiagnosticStore computeDiagnostics{true, "/unused"};
    span<const u8> GetComputeDiagnosticGuest(u64, span<u8>) const;
    std::string ComputeDiagnosticProfile() const;
};
struct Traits {
    struct Quirks {
        bool brokenMultithreadedPipelineCompilation{};
        std::string Summary() { return {}; }
    } quirks;
    bool CanUsePushDescriptors(span<vk::DescriptorSetLayoutBinding>) { return false; }
    std::string Summary() { return {}; }
};
struct Assembler { void WaitIdle() {} };
struct GPU {
    vk::raii::Device vkDevice{nullptr};
    vk::raii::PhysicalDevice vkPhysicalDevice{nullptr};
    Traits traits; std::optional<ShaderManager> shader;
    std::optional<Assembler> graphicsPipelineAssembler;
};
struct InterconnectContext { GPU &gpu; };
struct Pipeline {
    struct ShaderStage { vk::ShaderModule module; std::vector<u32> diagnosticSpirv; size_t diagnosticSpirvBytes{}; };
    struct CompiledPipeline {
        vk::raii::DescriptorSetLayout descriptorSetLayout;
        vk::raii::PipelineLayout pipelineLayout;
        vk::raii::Pipeline pipeline;
        bool usesPushDescriptors{};
    };
};
'''
with tempfile.TemporaryDirectory() as temporary:
    cpp = pathlib.Path(temporary) / "production.cpp"
    cpp.write_text(stubs + production + shader_production)
    (pathlib.Path(temporary) / "shader_compiler").symlink_to(root / "app/libraries/shader-compiler", target_is_directory=True)
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++20", "-fsyntax-only", "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS",
        "-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1", "-DVULKAN_HPP_NO_SETTERS",
        "-DVULKAN_HPP_NO_SMART_HANDLE", "-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0",
        "-I" + sys.argv[1], "-I" + str(root / "app/libraries/vkhpp/Vulkan-Headers/include"),
        "-I" + temporary, "-I" + str(root / "app/libraries/fmt/include"),
        "-I" + str(root / "app/src/main/cpp/skyline"), "-I" + str(root / "app/libraries"), str(cpp),
    ], check=True)
print("production compute creation boundary: Vulkan-Hpp syntax PASS")
