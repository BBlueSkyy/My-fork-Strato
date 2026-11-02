#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <skyline/gpu/diagnostic/pacman_triangle_capture.h>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
template<class T> using span = std::span<T>;
// Formatting is not under test. Production logging still executes.
namespace fmt {
    template<class... Args> std::string format(const char *, Args &&...) { return {}; }
    template<class T> const void *ptr(T *pointer) { return pointer; }
}
namespace vk {
    struct Rect2D {};
    enum class Format { eOther, eB10G11R11UfloatPack32 };
    using PipelineStageFlags = u32;
}
struct Texture {
    struct { u32 width, height; } dimensions;
    struct Format { vk::Format vkFormat; };
    std::shared_ptr<Format> format;
    u32 layout{};
    std::shared_ptr<int> cycle{std::make_shared<int>(0)};
};
struct TextureView {
    std::shared_ptr<Texture> texture;
    TextureView &operator*() { return *this; }
};
using SubpassFunction = std::function<void()>;
namespace node {
    struct NextSubpassFunctionNode {};
    struct SubpassFunctionNode {};
}
struct Node {
    bool next;
    SubpassFunction function;
    template<class T> Node(std::in_place_type_t<T>, SubpassFunction function)
        : next{std::is_same_v<T, node::NextSubpassFunctionNode>}, function{std::move(function)} {}
};
namespace diagnostic = skyline::gpu::diagnostic;

struct CommandExecutor {
    struct SubpassHooks { SubpassFunction before; };
    using SubpassHookFactory = std::function<SubpassHooks(u32)>;
    struct Slot { std::vector<Node> nodes, pendingPostRenderPassNodes; } storage;
    Slot *slot{&storage};
    u32 threshold{10000};
    struct Settings { u32 *executorFlushThreshold; } settings{&threshold};
    struct State { Settings *settings; } state{&settings};
    std::vector<std::function<void()>> flushCallbacks;
    std::vector<std::shared_ptr<int>> retainedResources;
    bool gotoNext{}, pacmanSawDepthPass{}, pacmanSawHdrScenePass{}, pacmanFlushingCallbacks{};
    std::uintptr_t pacmanLastHdrProducerIdentity{};
    std::array<std::uintptr_t, 8> pacmanTrackedTextures{};
    u32 pacmanTrackedTextureCount{};
    u64 pacmanPostCandidateSerial{}, submissionNumber{}, executionTag{41};
    u32 renderPassIndex{3}, rotations{}, resets{}, submitCalls{}, breaks{}, barriers{};
    std::shared_ptr<int> cycle{std::make_shared<int>(0)};
    std::string pacmanNextDrawSignature;

    bool CreateRenderPassWithSubpass(vk::Rect2D, span<TextureView *>, span<TextureView *>,
                                    span<TextureView *>, TextureView *, bool,
                                    vk::PipelineStageFlags, vk::PipelineStageFlags) { return gotoNext; }
    void Submit() {
        ++submitCalls;
        for (const auto &callback : flushCallbacks) callback();
        ++submissionNumber;
        ++executionTag;
        ++rotations;
        ++resets;
        slot->nodes.clear();
        retainedResources.clear();
    }
    void BreakRenderPass() { ++breaks; }
    void AddFullBarrier() { ++barriers; }
    void AddSubpass(SubpassFunction &&function, vk::Rect2D renderArea, span<TextureView *> sampledImages,
                    span<TextureView *> inputAttachments, span<TextureView *> colorAttachments,
                    TextureView *depthStencilAttachment, bool noSubpassCreation,
                    vk::PipelineStageFlags srcStageMask, vk::PipelineStageFlags dstStageMask,
                    SubpassHookFactory hookFactory = {});
};
#include "production.inc"

void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
TextureView MakeView(u32 width, u32 height, vk::Format format = vk::Format::eOther) {
    return {std::make_shared<Texture>(Texture{{width, height}, std::make_shared<Texture::Format>(Texture::Format{format})})};
}
struct Fixture {
    CommandExecutor executor;
    TextureView hdr{MakeView(1280, 720, vk::Format::eB10G11R11UfloatPack32)};
    TextureView post{MakeView(640, 360)};
    TextureView vi{MakeView(1920, 1080)};
    void Draw(TextureView &target, SubpassFunction draw = [] {}) {
        std::array<TextureView *, 1> colors{&target};
        executor.AddSubpass(std::move(draw), {}, {}, {}, colors, nullptr, false, 0, 0);
    }
    void Candidate(SubpassFunction draw = [] {}) { Draw(hdr); Draw(post, std::move(draw)); }
};

int main(int argc, char **argv) {
    try {
        Check(argc == 2, "missing temporary capture directory");
        const std::filesystem::path folder{argv[1]};
        auto &capture{diagnostic::PacmanCapture()};
        u32 passed{}, failed{};
        auto test{[&](const char *name, auto body) {
            try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
            catch (const std::exception &error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
        }};
        test("unarmed executor has no diagnostic intervention", [&] {
            Check(!capture.Active() && capture.ExecutorExperimentMode() == 0, "capture already armed");
            Fixture f;
            u32 calls{};
            f.executor.flushCallbacks.push_back([&] { ++calls; });
            for (u32 i{}; i < 8; ++i) f.Candidate();
            Check(calls == 0 && f.executor.submitCalls == 0 && f.executor.breaks == 0 && f.executor.barriers == 0,
                  "unarmed mode changed executor behavior");
        });
        test("mode16 refreshes only post slots3 and4 after queueing each draw", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f;
            std::vector<u64> selected;
            std::vector<std::size_t> queued;
            auto callbackResource{std::make_shared<int>(7)};
            std::weak_ptr<int> weakCallback{callbackResource};
            f.executor.flushCallbacks.push_back([&] {
                selected.push_back(f.executor.pacmanPostCandidateSerial);
                queued.push_back(f.executor.slot->nodes.size());
                f.executor.retainedResources.push_back(callbackResource);
                // Flush callbacks may publish query/deferred work.
                f.executor.slot->nodes.emplace_back(std::in_place_type<node::SubpassFunctionNode>, [] {});
            });
            auto drawResource{std::make_shared<int>(9)};
            std::weak_ptr<int> weakDraw{drawResource};
            for (u32 i{}; i < 4; ++i) f.Candidate([drawResource] {});
            Check(selected == std::vector<u64>{3, 4}, "expected callbacks exactly at candidates3 and4");
            Check(queued == std::vector<std::size_t>{6, 9}, "callback ran before current draw was queued or lost appended nodes");
            Check(f.executor.slot->nodes.size() == 10, "callback or draw nodes were discarded");
            Check(f.executor.submitCalls == 0 && f.executor.submissionNumber == 0 && f.executor.rotations == 0 &&
                  f.executor.resets == 0 && f.executor.executionTag == 41, "mode16 submitted, rotated, reset or changed executionTag");
            callbackResource.reset(); drawResource.reset();
            Check(!weakCallback.expired() && !weakDraw.expired(), "resources were released during callback refresh");
        });
        test("mode15 continues to submit only slots3 and4", [&] {
            capture.Start(folder, false, false, 15);
            Fixture f;
            std::vector<u64> selected;
            f.executor.flushCallbacks.push_back([&] { selected.push_back(f.executor.pacmanPostCandidateSerial); });
            for (u32 i{}; i < 8; ++i) f.Candidate();
            Check(selected == std::vector<u64>{3, 4, 7, 8} && f.executor.submitCalls == 4,
                  "existing mode15 selection/submission changed");
        });
        test("existing modes0 through15 keep their intervention counts", [&] {
            const std::array<u32, 16> expectedSubmits{0, 0, 0, 8, 0, 8, 0, 8, 2, 2, 2, 2, 0, 4, 4, 4};
            for (u32 mode{}; mode < expectedSubmits.size(); ++mode) {
                capture.Start(folder, false, false, mode);
                Fixture f;
                for (u32 i{}; i < 8; ++i) f.Candidate();
                Check(f.executor.submitCalls == expectedSubmits[mode], "existing mode submission count changed");
                Check(f.executor.breaks == (mode == 1 ? 16U : 0U), "existing split-mode count changed");
                Check(f.executor.barriers == (mode == 2 ? 16U : 0U), "existing barrier-mode count changed");
            }
        });
        test("callback-only reentry does not recursively invoke flush callbacks", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f;
            u32 calls{};
            f.executor.flushCallbacks.push_back([&] { ++calls; f.Candidate(); });
            for (u32 i{}; i < 3; ++i) f.Candidate();
            Check(calls == 1 && f.executor.pacmanPostCandidateSerial == 4,
                  "callback publication recursively refreshed candidate4 or no refresh occurred");
            Check(!f.executor.pacmanFlushingCallbacks && f.executor.slot->nodes.size() == 8,
                  "reentry guard leaked or callback-published draw was lost");
            Check(f.executor.submitCalls == 0, "callback reentry submitted");
        });
        test("callback exception propagates and releases the reentry guard", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f;
            f.executor.flushCallbacks.push_back([] { throw std::runtime_error("callback failure"); });
            f.Candidate(); f.Candidate();
            bool caught{};
            try { f.Candidate(); }
            catch (const std::runtime_error &error) { caught = std::string{error.what()} == "callback failure"; }
            Check(caught && !f.executor.pacmanFlushingCallbacks, "callback failure swallowed or guard left set");
            Check(f.executor.slot->nodes.size() == 6 && f.executor.submitCalls == 0,
                  "callback exception discarded queued work or submitted");
            u32 calls{};
            f.executor.flushCallbacks.clear();
            f.executor.flushCallbacks.push_back([&] { ++calls; });
            f.Candidate();
            Check(calls == 1, "next selected candidate was suppressed after callback exception");
        });
        test("natural threshold submits in mode16", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f; f.executor.threshold = 0;
            for (u32 i{}; i < 4; ++i) f.Candidate();
            Check(f.executor.submitCalls == 8 && f.executor.rotations == 8 && f.executor.executionTag == 49,
                  "mode16 suppressed ordinary natural submissions");
        });
        test("callback-appended work participates in the natural threshold", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f; f.executor.threshold = 6;
            bool published{};
            u32 callbackCalls{};
            f.executor.flushCallbacks.push_back([&] {
                ++callbackCalls;
                if (!published) {
                    published = true;
                    Check(f.executor.slot->nodes.size() == 6, "refresh did not start at threshold equality");
                    f.executor.slot->nodes.emplace_back(std::in_place_type<node::SubpassFunctionNode>, [] {});
                }
            });
            for (u32 i{}; i < 3; ++i) f.Candidate();
            Check(f.executor.submitCalls == 1 && callbackCalls == 2,
                  "callback node crossed threshold without a following natural Submit");
            Check(f.executor.slot->nodes.empty() && f.executor.executionTag == 42,
                  "natural Submit did not consume callback-appended work");
        });
        test("nested callback draws defer threshold submission until callback exits", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f; f.executor.threshold = 6;
            bool published{};
            u32 callbackCalls{};
            f.executor.flushCallbacks.push_back([&] {
                ++callbackCalls;
                if (!published) {
                    published = true;
                    f.Candidate();
                    Check(f.executor.submitCalls == 0, "nested AddSubpass submitted while callback bundle was active");
                    Check(f.executor.slot->nodes.size() == 8, "nested callback draw nodes were lost");
                }
            });
            for (u32 i{}; i < 3; ++i) f.Candidate();
            Check(f.executor.submitCalls == 1 && callbackCalls == 2 && !f.executor.pacmanFlushingCallbacks,
                  "nested threshold crossing did not submit once after callback completion");
            Check(f.executor.slot->nodes.empty(), "outer natural Submit left nested draws queued");
        });
        test("gotoNext protects mode16 refresh and mode15 submit", [&] {
            for (u32 mode : {15U, 16U}) {
                capture.Start(folder, false, false, mode);
                Fixture f; f.executor.gotoNext = true; f.executor.threshold = 0;
                u32 calls{};
                f.executor.flushCallbacks.push_back([&] { ++calls; });
                for (u32 i{}; i < 4; ++i) f.Candidate();
                Check(calls == 0 && f.executor.submitCalls == 0, "next-subpass path intervened");
            }
        });
        test("VI and unrelated color draws are not post candidates", [&] {
            capture.Start(folder, false, false, 16);
            Fixture f;
            u32 calls{}; f.executor.flushCallbacks.push_back([&] { ++calls; });
            for (u32 i{}; i < 4; ++i) { f.Draw(f.hdr); f.Draw(f.vi); }
            Check(f.executor.pacmanPostCandidateSerial == 0 && calls == 0, "VI draw was selected");
            f.executor.pacmanSawHdrScenePass = false;
            for (u32 i{}; i < 4; ++i) f.Draw(f.post);
            Check(f.executor.pacmanPostCandidateSerial == 0 && calls == 0, "unrelated color draw was selected");
        });
        test("mode16 persists after bounded60-present capture", [&] {
            capture.Start(folder, true, true, 16);
            Check(!capture.SnapshotsEnabled() && !capture.ScanEnabled(), "executor mode enabled image readbacks");
            Fixture f;
            u32 calls{}; f.executor.flushCallbacks.push_back([&] { ++calls; });
            for (u32 i{}; i < 4; ++i) f.Candidate();
            for (u32 i{}; i < 59; ++i) capture.CompletePresentation();
            Check(capture.Active(), "capture closed before 60 presents");
            capture.CompletePresentation();
            Check(!capture.Active() && capture.PresentedFrames() == 60 && capture.ExecutorExperimentMode() == 16,
                  "mode16 capture limit or persistence changed");
            std::ifstream metadataFile{folder / "capture_complete.txt"};
            const std::string metadata{std::istreambuf_iterator<char>{metadataFile}, {}};
            Check(metadata.find("executor_callback_refreshes=2\n") != std::string::npos,
                  "completion metadata did not count callback refreshes");
            Check(metadata.find("executor_experiment_draws=0\n") != std::string::npos,
                  "mode16 callback refreshes were counted as extra submits");
            for (u32 i{}; i < 4; ++i) f.Candidate();
            Check(calls == 4 && f.executor.submitCalls == 0, "callback intervention stopped when metadata capture closed");
        });
        std::cout << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
