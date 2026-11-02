// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <gpu/compute_diagnostics.h>

using skyline::gpu::ComputeDiagnosticStore;
static std::string Read(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path path{argv[1]};
    ComputeDiagnosticStore disabled{false, path};
    const std::vector<std::uint8_t> guest{0, 127, 255}, spirv{3, 2, 35, 7};
    assert(!disabled.Begin("identity", guest, spirv));
    assert(!disabled.Finish(-13, "ErrorUnknown"));
    assert(!std::filesystem::exists(path));
    ComputeDiagnosticStore store{true, path};
    assert(store.Begin("identity=first\n", guest, spirv));
    assert(Read(path / "guest.bin") == std::string("\0\x7f\xff", 3));
    assert(Read(path / "shader.spv") == std::string("\x03\x02\x23\x07", 4));
    assert(Read(path / "metadata.txt").find("status=pending") != std::string::npos);
    assert(store.Finish(-13, "ErrorUnknown"));
    assert(Read(path / "result.txt") == "vk_result=-13\nvk_result_name=ErrorUnknown\n");
    assert(store.Begin("identity=second\n", {}, spirv));
    assert(Read(path / "guest.bin").empty());
    assert(!std::filesystem::exists(path / "result.txt"));
    std::vector<std::uint8_t> oversized(ComputeDiagnosticStore::MaxShaderBytes + 1);
    assert(!store.Begin("identity=oversize\n", guest, oversized));
    assert(!std::filesystem::exists(path / "shader.spv"));
    assert(Read(path / "metadata.txt").find("status=omitted_size_limit") != std::string::npos);
    assert(store.Begin(std::string(ComputeDiagnosticStore::MaxMetadataBytes - 32, 'x'), guest, spirv));
    assert(Read(path / "metadata.txt").size() <= ComputeDiagnosticStore::MaxMetadataBytes);
    assert(!store.Begin(std::string(ComputeDiagnosticStore::MaxMetadataBytes + 1, 'x'), guest, spirv));
    assert(Read(path / "metadata.txt").size() < ComputeDiagnosticStore::MaxMetadataBytes);
    assert(!store.Begin("identity=incomplete\n", guest, {}, false));
    assert(!std::filesystem::exists(path / "shader.spv"));
    assert(Read(path / "metadata.txt").find("status=omitted_payload_unavailable") != std::string::npos);
    const auto blocked{path / "not-a-directory"};
    { std::ofstream file{blocked}; file << "blocked"; }
    ComputeDiagnosticStore ioFailure{true, blocked};
    assert(!ioFailure.Begin("identity", guest, spirv));
    assert(!ioFailure.Finish(-13, "ErrorUnknown"));
    std::cout << "compute diagnostic store: PASS\n";
}
