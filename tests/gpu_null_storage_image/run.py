#!/usr/bin/env python3
"""Check the null storage image policy: production header on the host plus source-level contract guards."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/cpp/skyline/gpu'
HERE = pathlib.Path(__file__).resolve().parent


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    sys.exit(1)


# 1. Production header, compiled and executed
with tempfile.TemporaryDirectory(prefix='strato-null-storage-image-') as directory:
    binary = pathlib.Path(directory) / 'tests'
    sanitize = [] if os.environ.get('SANITIZE') == '0' else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                    f'-I{GPU.parent}', str(HERE / 'tests.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# 2. Every path that hands out the shared null texture must say why, or consumers cannot tell stale
#    TICs from surfaces the guest expects to exist.
textures = (GPU / 'interconnect/common/textures.cpp').read_text()
get_texture = function(textures, 'TextureView *Textures::GetTexture(')
if get_texture.count('nullTextureView.get()') != 1 or 'CreateNullTexture(ctx)' not in get_texture:
    fail('GetTexture must hand out the null texture from exactly one place (the getNullTexture lambda)')
reasons = re.findall(r'getNullTexture\(NullTextureReason::(\w+)\)', get_texture)
if sorted(reasons) != ['PoolIndexOutOfRange', 'Unmapped', 'UnsupportedFormat']:
    fail(f'GetTexture fallbacks must each report their reason, found {reasons}')

# 3. The shared null texture is a sampled-only stand-in: it must never be widened into a storage image
#    (a writable shared 1x1 image leaks stores between bindings), storage goes through the policy instead.
null_texture = function(textures, 'static std::shared_ptr<TextureView> CreateNullTexture(')
if 'eStorage' in null_texture:
    fail('CreateNullTexture must not gain storage usage, use ResolveStorageImageNullAction / null descriptors')

pipeline = (GPU / 'interconnect/common/pipeline.inc').read_text()
image_binding = function(pipeline, 'GetImageBinding(InterconnectContext &ctx')
if 'ResolveStorageImageNullAction' not in image_binding:
    fail('GetImageBinding must consult ResolveStorageImageNullAction')
if 'AttachTexture' in image_binding[:image_binding.index('ctx.executor.AttachTexture(texture);')]:
    fail('the null texture must not be attached before the policy decision')

# 4. Maxwell's 0xFFFFFFFF bindless sentinel must become a complete sampled null
#    resource: valid sampler + null image view. A null sampler is not a Vulkan null descriptor.
sampled_binding = function(pipeline, 'GetTextureBinding(InterconnectContext &ctx')
required_sampled = ('IsNullBindlessHandle(handle.raw)', 'GetNullSampler(ctx)',
                    'supportsNullDescriptor', 'VK_NULL_HANDLE')
for token in required_sampled:
    if token not in sampled_binding:
        fail(f'sampled null bindless path is missing {token}')
if sampled_binding.index('GetNullSampler(ctx)') > sampled_binding.index('GetSampler(ctx, handle.samplerIndex'):
    fail('0xFFFFFFFF must select the valid null sampler before normal TSC lookup')

samplers = (GPU / 'interconnect/common/samplers.cpp').read_text()
null_sampler = function(samplers, 'vk::raii::Sampler *Samplers::GetNullSampler(')
if 'vk::Filter::eLinear' not in null_sampler or 'std::make_unique<vk::raii::Sampler>' not in null_sampler:
    fail('null bindless sampler must be a real deterministic Vulkan sampler')

if 'index == NullBindlessTextureIndex' not in textures:
    fail('shader metadata lookup must recognize the 0xFFFFF TIC half without warning')

# 5. A Vulkan null sampled image has no TextureView to track in render-pass usage.
#    The executor must skip those entries instead of dereferencing the null pointer.
executor = (GPU / 'interconnect/command_executor.cpp').read_text()
create_rp = function(executor, 'bool CommandExecutor::CreateRenderPassWithSubpass(')
if '!view || view->texture->ValidateRenderPassUsage' not in create_rp:
    fail('render-pass validation must accept null sampled image descriptors')
if not re.search(r'for \(auto view : sampledImages\)\s*if \(view\)\s*view->texture->UpdateRenderPassUsage', create_rp):
    fail('render-pass usage tracking must skip null sampled image descriptors')

# 6. The Turnip descriptor-pool diagnostic must stay dormant until a real
#    VK_ERROR_OUT_OF_POOL_MEMORY, and its polling thread must shut down cleanly.
executor_header = (GPU / 'interconnect/command_executor.h').read_text()
if 'EnableGpuCheckpoints{true}' not in executor_header:
    fail('descriptor-pool diagnostic build must compile checkpoint support')
if 'std::atomic<bool> stopping{}' not in executor_header or '~CheckpointPollerThread();' not in executor_header:
    fail('checkpoint poller must have cooperative shutdown')

checkpoint_impl = function(executor, 'u32 CommandExecutor::AddCheckpointImpl(')
if 'IsDescriptorPoolFailureCheckpointTracingArmed()' not in checkpoint_impl:
    fail('GPU checkpoints must remain runtime-gated until descriptor-pool failure')
if 'nextDiagnosticCheckpointId.fetch_add' not in checkpoint_impl:
    fail('diagnostic checkpoint IDs must be process-global')

if 'CheckpointPollerThread::~CheckpointPollerThread()' not in executor or 'thread.join()' not in executor:
    fail('checkpoint poller destructor must join its worker')
if 'GPUCheckpointDiagnostic: stalled' not in executor:
    fail('checkpoint poller must report a bounded stalled GPU marker')

descriptor_allocator = (GPU / 'descriptor_allocator.cpp').read_text()
if 'ArmDescriptorPoolFailureCheckpointTracing();' not in descriptor_allocator:
    fail('descriptor allocator must arm GPU tracing after out-of-pool memory')
if 'DescriptorPoolDiagnostic: allocation-failed' not in descriptor_allocator or 'DescriptorPoolDiagnostic: allocation-recovered' not in descriptor_allocator:
    fail('descriptor allocator must report failure and recovery state')

# 7. Draw stalls after descriptor-pool recovery must identify the exact pipeline
#    and draw parameters without logging every draw.
maxwell3d = (GPU / 'interconnect/maxwell_3d/maxwell_3d.cpp').read_text()
draw = function(maxwell3d, 'void Maxwell3D::Draw(')
for token in ('IsDescriptorPoolFailureCheckpointTracingArmed()',
              'PackedPipelineStateHash{}',
              'sourcePackedState.shaderHashes',
              'nullSampledImages',
              'Before draw {}',
              'After draw {}',
              'sampledDiagnosticCount',
              'view->range.baseMipLevel',
              'view->range.baseArrayLayer'):
    if token not in draw:
        fail(f'draw checkpoint diagnostic is missing {token}')

# 8. A sampled/attachment overlap must use VK_EXT_attachment_feedback_loop_layout
#    instead of issuing an invalid draw in VK_IMAGE_LAYOUT_GENERAL.
traits_h = (GPU / 'trait_manager.h').read_text()
gpu_cpp = (GPU.parent / 'gpu.cpp').read_text()
gpu_h = (GPU.parent / 'gpu.h').read_text()
compat_h = (GPU / 'vulkan_compat.h').read_text()
texture_cpp = (GPU / 'texture/texture.cpp').read_text()
assembler = (GPU / 'graphics_pipeline_assembler.cpp').read_text()
nodes_h = (GPU / 'interconnect/command_nodes.h').read_text()
nodes_cpp = (GPU / 'interconnect/command_nodes.cpp').read_text()
for token, source in (
    ('gpu/vulkan_compat.h', gpu_h),
    ('VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME', compat_h),
    ('1000339000', compat_h),
    ('0x00080000', compat_h),
    ('0x02000000', compat_h),
    ('0x04000000', compat_h),
    ('0x00000008', compat_h),
    ('supportsAttachmentFeedbackLoopLayout', traits_h),
    ('VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME', gpu_cpp),
    ('VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT', gpu_cpp),
    ('VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT', texture_cpp),
    ('VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT', assembler),
    ('VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT', assembler),
    ('AddFeedbackLoopDependency', nodes_h),
    ('VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT', nodes_cpp),
    ('VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT', maxwell3d),
    ('OverlapsSubresource', maxwell3d),
    ('feedbackTextureStates', maxwell3d),
    ('barrier.texture->GetBacking() == texture->GetBacking()', maxwell3d),
    ('pipeline->SyncDescriptors(', maxwell3d),
):
    if token not in source:
        fail(f'attachment feedback-loop support is missing {token}')

if 'SampledViewTypeMismatch:' not in pipeline or 'shaderHash = 0' not in sampled_binding:
    fail('cube sampled-view mismatch must remain diagnostic until its TIC cause is proven')

# e2DNoMipmap is physically one layer, but a shader may legally declare it as
# a 2D array. The view dimensionality must follow the shader without inflating
# the physical backing.
for token in (
    'case TextureImageControl::TextureType::e2DNoMipmap: {',
    'shaderType == Shader::TextureType::ColorArray2D',
    'guest.viewType = arrayView ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D',
    'guest.layerCount = 1',
):
    if token not in textures:
        fail(f'e2DNoMipmap shader-visible array view support is missing {token}')
for token in ('NullTextureReason sampledNullReason', 'sampledNullReason != NullTextureReason::None',
              'supportsNullDescriptor', '.imageView = VK_NULL_HANDLE'):
    if token not in sampled_binding:
        fail(f'invalid sampled TICs must use a Vulkan null image descriptor when available: missing {token}')
if 'Attachment feedback loop capability:' not in gpu_cpp:
    fail('feedback-loop capability must be reported once so device feature support is observable')

traits_cpp = (GPU / 'trait_manager.cpp').read_text()
for token, source in (
    ('brokenAttachmentFeedbackLoopLayout', traits_h),
    ('brokenAttachmentFeedbackLoopLayout = true', traits_cpp),
    ('!traits.quirks.brokenAttachmentFeedbackLoopLayout', gpu_cpp),
    ('Attachment feedback loop layout disabled by driver quirk', gpu_cpp),
):
    if token not in source:
        fail(f'Turnip feedback-loop disable policy is missing {token}')

# 9. DevDebug must use a modern Khronos Validation Layer and enable
#    Synchronization Validation programmatically. Release remains unaffected by
#    the NDEBUG guard and the workflow installs the layer only before DevDebug.
workflow = (ROOT / '.github/workflows/pr_build.yml').read_text()
for token, source in (
    ('VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT', gpu_cpp),
    ('VkValidationFeaturesEXT', gpu_cpp),
    ('.pNext = instancePNext', gpu_cpp),
    ('synchronization=true (DevDebug)', gpu_cpp),
    ('android-binaries-1.4.363.1.zip', workflow),
    ('2c17b00a95b18fd63244b27332ef86f45c9269769f798a6261ce80b869c41af4', workflow),
    ('Install Khronos Validation Layer 1.4.363.1', workflow),
):
    if token not in source:
        fail(f'modern synchronization-validation diagnostic is missing {token}')

print('null storage/sampled image source contract: ok')
