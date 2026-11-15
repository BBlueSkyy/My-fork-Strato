#!/usr/bin/env python3
"""Opt-in VI source capture for exact pre/post #341 Strato source trees.

This strictly diagnostic injection uses the same established Vulkan readback idiom
as PR #445, but does not claim to capture intermediary HDR/bloom passes.
Fail if the known insertion point changes.
"""
from pathlib import Path
import re
import sys

p = Path("app/src/main/cpp/skyline/gpu/presentation_engine.cpp")
s = p.read_text()
assert s.count('        texture->SynchronizeHost();') == 1, "Unknown historical PresentFrame() layout"
assert 'gpu_capture' not in s and 'MK8_PR341_VI_CAPTURE' not in s, "Unexpected existing instrumentation"
assert s.count('#include <gpu.h>') == 1

s = s.replace('#include <gpu.h>', '#include <gpu.h>\n#include <os.h>\n#include <filesystem>\n#include <fstream>\n#include <system_error>\n#include <cstdint>', 1)
block = r'''
        // MK8_PR341_VI_CAPTURE: identical, explicitly armed, bounded VI readback
        // for the pre341 and post341 *historic* code trees. No GPU behavior is
        // changed in normal execution and no intermediates are intercepted.
        {
            namespace fs = std::filesystem;
            static unsigned mk8ViPoll{};
            static unsigned mk8ViRemaining{};
            static fs::path mk8ViDestination{};
            static unsigned mk8ViWritten{};
            static unsigned mk8ViFailed{};
            if (!mk8ViRemaining && (++mk8ViPoll % 16 == 1)) {
                const fs::path root{fs::path(state.os->publicAppFilesPath) / "gpu_capture" / "mk8_glow"};
                std::error_code ec;
                fs::create_directories(root, ec);
                if (ec) {
                    LOGW("MK8_PR341_VI_CAPTURE cannot create {}: {}", root.string(), ec.message());
                } else if (fs::exists(root / "ARM_CAPTURE", ec) && !ec) {
                    ec.clear();
                    fs::remove(root / "ARM_CAPTURE", ec);
                    if (ec) {
                        LOGW("MK8_PR341_VI_CAPTURE cannot disarm ARM_CAPTURE: {}", ec.message());
                    } else {
                        for (unsigned candidateIndex{1}; candidateIndex <= 999; ++candidateIndex) {
                            auto candidate{root / fmt::format("capture_{:03}", candidateIndex)};
                            ec.clear();
                            if (fs::exists(candidate, ec) || ec)
                                continue;
                            if (!fs::create_directories(candidate, ec) || ec)
                                continue;
                            mk8ViDestination = candidate;
                            mk8ViRemaining = 4;
                            mk8ViWritten = mk8ViFailed = 0;
                            std::ofstream readme(candidate / "README.txt");
                            if (readme)
                                readme << "MK8 historical pre/post PR341 VI source capture\n"
                                          "Output: present_source_XX_*.raw and present_XX.txt\n"
                                          "Captures 4 presentation sources only. This is NOT the full\n"
                                          "PR445 HDR mini-RenderDoc. No intermediate HDR/bloom readbacks,\n"
                                          "draw events, shader IDs, or pass_readbacks_done.txt are produced.\n"
                                          "Do not infer the first corrupt HDR stage from these VI frames.\n"
                                          "Keep the same driver, game version, resolution and camera.\n";
                            if (fs::exists(root / "FOCUS_MK8_POSTPROCESS", ec) && !ec) {
                                std::ofstream note(candidate / "HDR_FOCUS_UNSUPPORTED.txt");
                                if (note)
                                    note << "FOCUS_MK8_POSTPROCESS is not implemented in this historical VI-only capture.\n";
                            }
                            LOGI("MK8_PR341_VI_CAPTURE armed: {}", candidate.string());
                            break;
                        }
                    }
                } else if (!ec) {
                    // Create the folder even before the user arms the capture.
                    const auto markerHelp{root / "ARM_CAPTURE.rename_this_file_when_ready"};
                    ec.clear();
                    if (!fs::exists(markerHelp, ec) && !ec) {
                        std::ofstream hint(markerHelp);
                        if (hint)
                            hint << "Rename to ARM_CAPTURE while the target MK8 scene is visible.\n";
                    }
                }
            }

            if (mk8ViRemaining && !mk8ViDestination.empty()) {
                const auto number{4U - mk8ViRemaining};
                const auto valid{
                    texture->layout != vk::ImageLayout::eUndefined &&
                    texture->levelCount == 1 && texture->layerCount == 1 &&
                    texture->sampleCount == vk::SampleCountFlagBits::e1 &&
                    texture->surfaceSize > 0 && texture->surfaceSize <= 12ULL * 1024 * 1024 &&
                    (texture->format->vkAspect & vk::ImageAspectFlagBits::eColor) &&
                    !texture->format->IsCompressed()
                };
                const auto filename{fmt::format("present_source_{:02}_{}x{}_{}.raw",
                    number, texture->dimensions.width, texture->dimensions.height,
                    vk::to_string(texture->format->vkFormat))};
                bool wrote{};
                std::ofstream meta(mk8ViDestination / fmt::format("present_{:02}.txt", number));
                if (meta)
                    meta << fmt::format("present_index={} source_frame_id={} width={} height={} format={} "
                        "layout={} bytes={} eligible={} diagnostic=MK8_PR341_VI_CAPTURE\n",
                        number, frame.id, texture->dimensions.width, texture->dimensions.height,
                        vk::to_string(texture->format->vkFormat), vk::to_string(texture->layout),
                        texture->surfaceSize, valid);

                if (valid) {
                    // Same bounded Vulkan copy and fence wait as PR #445's VI path.
                    // The readback is synchronous only while explicitly armed.
                    try {
                        if (texture->cycle)
                            texture->cycle->Wait();
                        auto staging{gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
                        auto captureCycle{gpu.scheduler.Submit(
                            [texture, staging](vk::raii::CommandBuffer &commandBuffer) {
                                texture->CopyIntoStagingBuffer(commandBuffer, staging);
                            })};
                        captureCycle->AttachObjects(texture, staging);
                        captureCycle->Wait();
                        std::ofstream raw(mk8ViDestination / filename, std::ios::binary | std::ios::trunc);
                        if (raw) {
                            raw.write(reinterpret_cast<const char *>(staging->data()),
                                      static_cast<std::streamsize>(staging->size()));
                            raw.flush();
                            wrote = raw.good();
                        }
                    } catch (const std::exception &e) {
                        LOGE("MK8_PR341_VI_CAPTURE readback error: {}", e.what());
                    }
                }
                if (wrote) {
                    ++mk8ViWritten;
                    LOGI("MK8_PR341_VI_CAPTURE wrote {}", filename);
                } else {
                    ++mk8ViFailed;
                    LOGW("MK8_PR341_VI_CAPTURE missed VI source {}", number);
                }
                if (--mk8ViRemaining == 0) {
                    std::ofstream done(mk8ViDestination / "capture_complete.txt");
                    if (done)
                        done << "vi_only=true\nframes=4\nwritten=" << mk8ViWritten
                             << "\nfailed=" << mk8ViFailed << "\n";
                    LOGI("MK8_PR341_VI_CAPTURE finished: written={} failed={}",
                         mk8ViWritten, mk8ViFailed);
                }
            }
        }
'''
s=s.replace('        texture->SynchronizeHost();','        texture->SynchronizeHost();\n'+block,1)
assert s.count("MK8_PR341_VI_CAPTURE") >= 9
assert s.count("texture->CopyIntoStagingBuffer") == 1
p.write_text(s)

# The historical pre341 Texture kept GPU staging copy private to
# CommandExecutor. The post341 tree already grants PresentationEngine access.
# Add only that source-level diagnostic friendship where missing; this changes
# no Texture layout, rendering path or existing Vulkan operation.
h = Path("app/src/main/cpp/skyline/gpu/texture/texture.h")
hs = h.read_text()
class_decl = "    class Texture : public std::enable_shared_from_this<Texture> {"
friend_decl = "        friend class interconnect::CommandExecutor;"
assert hs.count(class_decl) == 1, "Unexpected historical Texture declaration"
assert hs.count(friend_decl) == 1, "Unexpected historical Texture friendship"
class_body = hs.split(class_decl, 1)[1].split("      private:", 1)[0]
if "friend class PresentationEngine;" not in class_body:
    hs = hs.replace(class_decl + "\n" + friend_decl,
                    class_decl + "\n" + friend_decl + "\n        friend class PresentationEngine;", 1)
    h.write_text(hs)
    print(f"Added diagnostic PresentationEngine friendship to {h}")
else:
    print("Historical Texture already permits PresentationEngine staging copy")

assert "friend class PresentationEngine;" in hs.split(class_decl, 1)[1].split("      private:", 1)[0]
print(f"Instrumented {p} ({len(s)} bytes); VI only. No HDR pass capture.")
