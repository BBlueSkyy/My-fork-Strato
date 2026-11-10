#!/usr/bin/env python3
"""Compile actual promotion/view/cache methods against real Vulkan resources.

Only allocator, scheduler and fence interfaces are host facades. Allocation,
image views, copy commands and fences use the Vulkan ICD and validation layer.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature, default_arguments=False, constructor=False):
    start = text.index(signature)
    end = (text.index(" {", start) + 1 if constructor else (text.index(") {", start) + 2 if default_arguments else text.index("{", start))) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


sys.path.insert(0, str(ROOT / "tests/gpu_gmmu_segment"))

with tempfile.TemporaryDirectory(prefix="strato-backing-promotion-") as directory:
    directory = pathlib.Path(directory)
    # This test-only header copy activates automatic manager registration while
    # production remains disabled until the combined release gate is proven.
    if os.environ.get("TEXMAN_MIXED_ACTIVATION_TEST"):
        gate_header = (GPU / "texture/region_authority.h").read_text()
        assert "MixedRegionAuthorityEnabled{false}" in gate_header or "MixedRegionAuthorityEnabled{true}" in gate_header
        gate_header = gate_header.replace("MixedRegionAuthorityEnabled{false}", "MixedRegionAuthorityEnabled{true}")
        if os.environ.get("TEXMAN_MIXED_GATE_OFF"):
            gate_header = gate_header.replace("MixedRegionAuthorityEnabled{true}", "MixedRegionAuthorityEnabled{false}")
        gate_copy = directory / "gpu/texture/region_authority.h"
        gate_copy.parent.mkdir(parents=True, exist_ok=True)
        gate_copy.write_text(gate_header.replace('#include "alias_group.h"', '#include <gpu/texture/alias_group.h>'))
    address_header = (ROOT / "app/src/main/cpp/skyline/common/address_space.h").read_text()
    address_source = (ROOT / "app/src/main/cpp/skyline/common/address_space.inc").read_text()
    def address_function(source, signature, defaults=False):
        start = source.index(signature)
        brace = source.index(") {", start) + 2 if defaults else source.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]
    (directory / "gmmu_translation.inc").write_text(address_function(address_header,"std::pair<span<u8>, size_t> LookupBlockLocked(",True)+"\n"+
        address_function(address_header,"TranslatedAddressRange TranslateRange(",True))
    (directory / "gmmu_identity.inc").write_text(address_function(address_header,"bool SameMappingLocked(")+"\n"+
        address_function(address_header,"std::vector<std::pair<VaType, VaType>> ChangedMappingRangesLocked(")+"\n"+
        address_function(address_header,"TrackedTranslation TranslateRangeTracked(",True)+"\n"+
        address_function(address_header,"void Map(",True)+"\n"+address_function(address_header,"void Unmap(",True))
    (directory / "gmmu_slow.inc").write_text(address_function(address_source,"MM_MEMBER(TranslatedAddressRange)::TranslateRangeImpl("))
    (directory / "gmmu_map.inc").write_text(address_function(address_source,"MAP_MEMBER(void)::MapLocked(")+"\n"+
        address_function(address_source,"MAP_MEMBER(void)::UnmapLocked("))
    fixture=(ROOT/"tests/gpu_gmmu_identity/tests.cpp").read_text()
    fixture=fixture[fixture.index("using skyline::MappingIdentity;"):fixture.index("int main()")]
    for original in ("translation", "identity", "slow", "map"):
        fixture=fixture.replace('"'+original+'.inc"','"gmmu_'+original+'.inc"')
    (directory/"gmmu_fixture.inc").write_text("namespace translation_test {\n"+fixture+"\n}\n")
    sys.path.insert(0, str(ROOT / "tests/gpu_gmmu_segment"))
    from gmmu_segment_fixture import write_fixture
    write_fixture(directory, ref=os.environ.get("TEXMAN_SEGMENT_BASELINE_REF"))
    source = (GPU / "texture/texture.cpp").read_text()
    baseline = os.environ.get("TEXMAN_BASELINE_VIEWS_REF")
    view_source = subprocess.check_output(["git", "show", baseline + ":app/src/main/cpp/skyline/gpu/texture/texture.cpp"],
        cwd=ROOT, text=True) if baseline else source
    # The storage-identity view API from PR #383 adds an optional bool. Match
    # the exact production definition under test, including older baselines.
    view_api = function(view_source, "vk::ImageView TextureView::GetView(").split("{", 1)[0].strip()
    view_api = view_api.replace("TextureView::", "").replace("(bool identityMapping)", "(bool identityMapping = false)")
    (directory / "view_api.inc").write_text(view_api + ";\n")
    # Optional negative control: replace only the two view APIs with their
    # actual baseline bodies. The current bounds test must then fail.
    (directory / "production.inc").write_text("\n".join(function(
        view_source if signature in ("std::shared_ptr<TextureView> Texture::GetView(", "vk::ImageView TextureView::GetView(") else source,
        signature) for signature in (
        "bool Texture::LockWithTag(",
        "void Texture::unlock(",
        "void Texture::SetupGuestMappings(",
        "bool Texture::CanMaterializeGuestLayout(",
        "bool Texture::IsGuestLayoutReversible(",
        "bool Texture::CanReconcileGuestLayout(",
        "std::shared_ptr<texture::AliasGroup> Texture::RegisterAliases(",
        "void Texture::RequireAliasCoherence(",
        "void Texture::ReconcileAliases(",
        "void Texture::PrepareRegionGpuWrite(",
        "void Texture::RecordGpuWrite(",
        "void Texture::ValidateAliasWrites(",
        "void Texture::MarkGpuDirty(",
        "bool Texture::ValidateRenderPassUsage(",
        "void Texture::UpdateRenderPassUsage(",
        "bool Texture::ViewFormatAdmitted(",
        "bool Texture::ViewRangeFits(",
        "bool Texture::CanGrowMipLevels(",
        "std::shared_ptr<void> Texture::PinGuestMemoryForMipGrowth(",
        "std::shared_ptr<void> Texture::PinGuestMemory(",
        "void Texture::FreeGuest(",
        "boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies(",
        "void Texture::CopyFromStagingBuffer(",
        "void Texture::CopyIntoStagingBuffer(",
        "void Texture::CopyToGuest(",
        "std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl(",
        "void Texture::SynchronizeHost(",
        "void Texture::SynchronizeHostInline(",
        "void Texture::SynchronizeGuest(",
        "void Texture::PromoteBackingForView(",
        "std::shared_ptr<TextureView> Texture::GetView(",
        "vk::ImageUsageFlags TextureView::GetUsage(",
        "vk::ImageView TextureView::GetView(",
        "void Texture::SwapBacking(")))
    region_source = (GPU / "texture/region_authority.inc").read_text()
    if os.environ.get("TEXMAN_WITHDRAWAL_LINK_NEGATIVE"):
        branch = "if (!buffers.empty()) {\n            // A live mixed component cannot return to untracked legacy use."
        assert region_source.count(branch) == 1
        region_source = region_source.replace(branch, "if (false) {\n            // Test-only negative control: omit the persistent replacement.")
    (directory / "region.inc").write_text(region_source)
    (directory / "mixed_region.inc").write_text((GPU / "texture/mixed_region_authority.inc").read_text())
    buffer_regions = (GPU / "buffer_regions.inc").read_text()
    if os.environ.get("TEXMAN_BRIDGE_BUDGET_NEGATIVE"):
        guard = 'if (group->guestBridge && regions.size() >= texture::GuestBridgeMaxRegions) throw exception("Guest bridge region budget exceeded during coalesce");'
        assert buffer_regions.count(guard) == 1
        buffer_regions = buffer_regions.replace(guard, "/* test-only negative control: omitted budget guard */")
    (directory / "buffer_regions.inc").write_text(buffer_regions)
    buffer_header = (GPU / "buffer.h").read_text()
    (directory / "buffer_class.inc").write_text(buffer_header[buffer_header.index("    struct BufferBinding"):buffer_header.rindex("}")].replace("      private:", "      public:"))
    buffer_source = (GPU / "buffer.cpp").read_text()
    signatures = ["void Buffer::ResetMegabufferState(", "void Buffer::SetupStagedTraps(",
        "void Buffer::EnableTrackedShadowDirect(", "span<u8> Buffer::BeginWriteCpuSequencedDirect(",
        "bool Buffer::RefreshGpuReadsActiveDirect(", "bool Buffer::RefreshGpuWritesActiveDirect(",
        "bool Buffer::ValidateMegaBufferViewImplDirect(", "bool Buffer::ValidateMegaBufferViewImplStaged(", "bool Buffer::ValidateMegaBufferView(",
        "void Buffer::CopyFromImplDirect(", "void Buffer::CopyFromImplStaged(", "bool Buffer::WriteImplDirect(", "bool Buffer::WriteImplStaged(",
        "void Buffer::ReadImplDirect(", "void Buffer::ReadImplStaged(", "void Buffer::MarkGpuDirtyImplDirect(", "void Buffer::MarkGpuDirtyImplStaged(", "void Buffer::MarkGpuDirtyImpl(",
        "Buffer::Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, GuestBuffer", "Buffer::Buffer(LinearAllocatorState<> &delegateAllocator, GPU &gpu, vk::DeviceSize",
        "void Buffer::MarkGpuDirty(", "void Buffer::WaitOnFence(", "bool Buffer::PollFence(", "void Buffer::Invalidate(", "void Buffer::SynchronizeHost(", "bool Buffer::SynchronizeGuest(",
        "void Buffer::SynchronizeGuestImmediate(", "void Buffer::Read(", "bool Buffer::Write(", "void Buffer::CopyFrom(", "bool Buffer::TryMappedWrite(", "bool Buffer::TryMappedCopy(", "BufferView Buffer::GetView(", "BufferView Buffer::TryGetView(",
        "void Buffer::AdvanceSequence(", "span<u8> Buffer::GetReadOnlyBackingSpan(", "void Buffer::lock(", "bool Buffer::LockWithTag(", "void Buffer::unlock(", "bool Buffer::try_lock(",
        "BufferDelegate::BufferDelegate(", "Buffer *BufferDelegate::GetBuffer(", "void BufferDelegate::Link(", "vk::DeviceSize BufferDelegate::GetOffset(",
        "BufferBinding BufferView::GetBinding(", "void BufferView::Read(", "bool BufferView::Write(", "void BufferView::CopyFrom(",
        "BufferView::BufferView()", "BufferView::BufferView(BufferDelegate", "Buffer *BufferView::GetBuffer(", "vk::DeviceSize BufferView::GetOffset("]
    (directory / "buffer_production.inc").write_text("\n".join(function(buffer_source, signature, constructor=True) if "::Buffer(" in signature or "::BufferView(" in signature or "::BufferDelegate(" in signature else function(buffer_source, signature) for signature in signatures))
    common = ROOT / "app/src/main/cpp/skyline/common"
    spin = (common / "spin_lock.h").read_text()
    (directory / "buffer_support.inc").write_text(function(spin, "class SpinLock") + ";\n" + function(spin, "class RecursiveSpinLock") + ";\n" +
        "void SpinLock::LockSlow() { while (!try_lock()) std::this_thread::yield(); }\n" +
        "template<typename SizeType>\n" + function((common / "interval_list.h").read_text(), "class IntervalList") + ";\n")
    trap_source = (ROOT / "app/src/main/cpp/skyline/common/trap_manager.cpp").read_text()
    (directory / "trap_handler.inc").write_text(function(trap_source,"bool TrapManager::HandleTrap(").replace("TrapManager::HandleTrap", "HostTrap::HandleTrap"))
    buffer_manager = (GPU / "buffer_manager.cpp").read_text()
    (directory / "buffer_manager_class.inc").write_text(function((GPU / "buffer_manager.h").read_text(), "class BufferManager").replace("      private:", "      public:") + ";\n")
    (directory / "buffer_manager.inc").write_text(buffer_manager[buffer_manager.index("    BufferManager::BufferManager("):buffer_manager.rindex("}")])

    layout = (GPU / "texture/layout.cpp").read_text()
    header = (GPU / "texture/texture.h").read_text()
    (directory / "layout.inc").write_text(function(header, "struct MipLevelLayout") + ";\n" +
        "\n".join(function(layout, signature) for signature in (
            "size_t GetBlockLinearWidthGobs(",
            "template<typename Type>\n    constexpr Type CalculateBlockGobs(",
            "std::vector<MipLevelLayout> GetBlockLinearMipLayout(",
            "template<bool BlockLinearToPitch>\n    void CopyBlockLinearInternal(",
            "void CopyBlockLinearToLinear(Dimensions dimensions",
            "void CopyLinearToBlockLinear(Dimensions dimensions")).replace(
                "size_t tileWidthSpacing)", "size_t tileWidthSpacing = 0)"))
    (directory / "guest_copy.inc").write_text("namespace texture {\n" +
        "\n".join(function(layout, signature) for signature in (
            "void CopyBlockLinearToLinear(const GuestTexture &",
            "void CopyLinearToBlockLinear(const GuestTexture &",
            "void CopyPitchLinearToLinear(", "void CopyLinearToPitchLinear(")) + "\n}\n")
    (directory / "guest_size.inc").write_text("\n".join(function(source, signature).replace("GuestTexture::", "") for signature in (
        "size_t GuestTexture::GetSize(", "u32 GuestTexture::GetBackingLayerCount(",
        "u32 GuestTexture::GetViewLayerCount(", "u32 GuestTexture::GetViewDepth(")))
    (directory / "sizes.inc").write_text("\n".join(function(source, signature) for signature in (
        "size_t CalculateLevelStride(", "size_t CalculateTargetLevelStride(")))
    cache = (GPU / "cache/framebuffer_cache.cpp").read_text()
    (directory / "cache.inc").write_text(function(cache,
        "std::vector<vk::raii::Framebuffer> FramebufferCache::RetireViews("))
    nodes = (GPU / "interconnect/command_nodes.cpp").read_text()
    (directory / "attachment.inc").write_text(function(nodes,
        "attachmentInfo.push_back(vk::FramebufferAttachmentImageInfo") + ");")
    (directory / "scheduler_submit.inc").write_text(function((GPU / "command_scheduler.h").read_text(),
        "template<typename RecordFunction>", default_arguments=True))
    # Compile the exact full-match dimension predicate from 2c293491 too.
    manager = (GPU / "texture_manager.cpp").read_text()
    start = manager.index("(matchGuestTexture.dimensions.width")
    end = manager.index("&& matchGuestTexture.GetImageType()", start)
    (directory / "manager_class.inc").write_text(function((GPU / "texture_manager.h").read_text(), "class TextureManager").replace("      private:", "      public:") + ";\n")
    manager_baseline = os.environ.get("TEXMAN_BASELINE_MANAGER_REF")
    if manager_baseline:
        secondary = subprocess.run(["git", "show", manager_baseline + ":app/src/main/cpp/skyline/gpu/texture/secondary_overlap.h"],
            cwd=ROOT, text=True, capture_output=True)
        if secondary.returncode == 0:
            helper = directory / "gpu/texture/secondary_overlap.h"
            helper.parent.mkdir(parents=True, exist_ok=True)
            helper.write_text(secondary.stdout)
    manager_runtime = subprocess.check_output(["git","show",manager_baseline+":app/src/main/cpp/skyline/gpu/texture_manager.cpp"],cwd=ROOT,text=True) if manager_baseline else manager
    (directory / "manager.inc").write_text(function(manager_runtime,"TextureManager::TextureManager(")+"\n"+function(manager,"void TextureManager::RegisterBufferRegions(")+"\n"+function(manager_runtime,"void TextureManager::SynchronizeGuestOverlaps(")+"\n"+function(manager_runtime,"std::shared_ptr<TextureView> TextureManager::FindOrCreate("))
    (directory / "depth_offset.inc").write_text(function(layout,"std::optional<size_t> GetBlockLinearDepthSliceOffset("))
    (directory / "extent.inc").write_text("return " + manager[start:end].strip() + ";")
    start = manager.index("if (matchGuestTexture.format->IsCompatible")
    end = manager.index(") {", start)
    (directory / "match.inc").write_text(manager[manager.index("const bool sameTileLayout"):start] +
                                         "return " + manager[start + 3:end + 1] + ";")
    (directory / "selection.inc").write_text(function(manager, "else if (fullMatch)"))
    (directory / "request_range.inc").write_text(function(manager, "const vk::ImageSubresourceRange requestedRange") + ";")
    (directory / "request_validation.inc").write_text(function(manager, "if (!texture::SubresourceRangeFits("))
    (directory / "unrepresentable.inc").write_text(function(manager, "if (unrepresentableMatch || separateGroup)"))
    texture_cache_source = (GPU / "interconnect/common/textures.cpp").read_text()
    cache_begin = texture_cache_source.index("        if (textureHeaderCache.size()", texture_cache_source.index("TextureView *Textures::GetTexture("))
    cache_end = texture_cache_source.index("        if (index >= textureHeaders.size())", cache_begin)
    cache_prefix = texture_cache_source[cache_begin:cache_end]
    if os.environ.get("TEXMAN_MAPPING_CACHE_BASELINE"):
        cache_prefix = cache_prefix.replace("textureHeaderCache[index].texture.Current()", "static_cast<bool>(textureHeaderCache[index].texture.view)")
    (directory / "mapping_cache_lookup.inc").write_text(cache_prefix)
    rt_source=(GPU/"interconnect/maxwell_3d/pipeline_state.cpp").read_text()
    rt_refresh=function(rt_source,"void PipelineState::RefreshTextureMappings(")
    if os.environ.get("TEXMAN_MAPPING_RT_BASELINE"):
        rt_refresh="void PipelineState::RefreshTextureMappings(InterconnectContext &ctx) { (void)ctx; }"
    (directory/"mapping_rt.inc").write_text(rt_refresh+"\n"+
        function(rt_source,"std::shared_ptr<TextureView> PipelineState::GetColorRenderTargetForClear(")+"\n"+
        function(rt_source,"std::shared_ptr<TextureView> PipelineState::GetDepthRenderTargetForClear("))
    pool_lookup=function(texture_cache_source,"span<TextureImageControl> Textures::GetTextureHeaders(")
    if os.environ.get("TEXMAN_MAPPING_POOL_BASELINE"):
        pool_lookup="span<TextureImageControl> Textures::GetTextureHeaders(InterconnectContext &ctx) { return texturePool.UpdateGet(ctx).textureHeaders; }"
    (directory/"mapping_pool.inc").write_text(function(texture_cache_source,"void TexturePoolState::Flush(")+"\n"+pool_lookup)
    for stem, output in (("maxwell_dma", "dma"), ("inline2memory", "inline")):
        header=(GPU / "interconnect" / (stem+".h")).read_text()
        (directory/(output+"_class.inc")).write_text(function(header,"class "+("MaxwellDma" if output=="dma" else "Inline2Memory")).replace("      private:", "      public:")+";\n")
        body=(GPU / "interconnect" / (stem+".cpp")).read_text()
        (directory/(output+"_entrypoints.inc")).write_text(body[body.index("namespace skyline::gpu::interconnect"):])
    (directory/"cbuf_load.inc").write_text(function((GPU/"interconnect/maxwell_3d/constant_buffers.cpp").read_text(),"void ConstantBuffers::Load("))
    executor = (GPU / "interconnect/command_executor.cpp").read_text()
    (directory / "alias_retry.inc").write_text(function(executor,"void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &"))
    (directory / "graph.inc").write_text(function(executor,"static bool ViewsEqual(") + "\n" +
        function(executor,"bool CommandExecutor::CreateRenderPassWithSubpass(") + "\n" +
        function(executor,"void CommandExecutor::AddSubpass("))
    (directory / "region_hold.inc").write_text(function(executor,"void CommandExecutor::TrackTextureMapping(")+"\n"+function(executor,"void CommandExecutor::StartTextureMapping(")+"\n"+function(executor,"void CommandExecutor::TrackRegionUse(")+"\n"+function(executor,"void CommandExecutor::TrackBufferRegionUse(")+"\n"+function(executor,"void CommandExecutor::PrepareBufferWrite("))
    (directory / "region_publication.inc").write_text(function(executor,"CommandExecutor::RegionPublication CommandExecutor::AcquireRegionPublication("))
    (directory / "region_operation.inc").write_text(function((GPU / "interconnect/command_executor.h").read_text(),
        "template<typename Operation>\n        void RunWithTextureBackingRetry("))
    (directory / "preflight.inc").write_text(function(executor,"void CommandExecutor::PreflightTextureUses("))
    (directory / "commit.inc").write_text(function(executor,"void CommandExecutor::CommitTextureWrites(")+"\n"+function(executor,"void CommandExecutor::AddOutsideRpCommand("))
    draw = (GPU / "interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
    refresh=function(draw,"void Maxwell3D::RefreshTextureMappings(")
    (directory / "epoch.inc").write_text(refresh[refresh.index("{")+1:-1])
    (directory / "attach.inc").write_text(function(executor,"bool CommandExecutor::AttachTexture(")+"\n"+function(executor,"bool CommandExecutor::AttachBuffer(")+"\n"+function(executor,"void CommandExecutor::AttachLockedBuffer(")+"\n"+function(executor,"void CommandExecutor::AttachLockedBufferView("))
    (directory / "retry.inc").write_text(function(executor,
        "void CommandExecutor::HandleTextureBackingPromotion("))
    # GCC 14 deprecates shared_ptr atomic free functions required by Android libc++; keep other diagnostics fatal.
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-Wno-deprecated-declarations", "-Wno-unknown-pragmas", "-Wno-mismatched-tags", "-Wno-missing-field-initializers", "-Wno-unused-parameter", "-Wno-reorder-ctor", "-fsanitize=" + os.environ.get("SANITIZE", "undefined"),
                    "-fno-sanitize-recover=all", "-pthread", "-I" + str(directory),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
                    "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
                    "-I" + os.environ.get("BOOST_INCLUDE_DIR", "/usr/include"),
                    str(ROOT / "tests/gpu_backing_promotion/tests.cpp"),
                    os.environ.get("VULKAN_LOADER_LIBRARY", "-lvulkan"),
                    "-o", str(directory / "tests")] + shlex.split(os.environ.get("CXXFLAGS", "")), check=True)
    subprocess.run([str(directory / "tests")], check=True)
