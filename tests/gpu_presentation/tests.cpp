#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define LOGD(...) ((void)0)
using u32 = uint32_t;
using u8 = uint8_t;
using i32 = int32_t;
using jobject = void *;
template<class... T> auto exception(const char *message, T...) { return std::runtime_error(message); }
template<class T> struct span {
    span(T *, size_t) {}
    std::string as_string(bool) const { return {}; }
};
constexpr int AndroidNativeWindowMagic = 42;
constexpr int NATIVE_WINDOW_SET_CROP = 1, NATIVE_WINDOW_SET_SCALING_MODE = 2;
constexpr int NATIVE_WINDOW_SET_BUFFERS_TRANSFORM = 3, NATIVE_WINDOW_ENABLE_FRAME_TIMESTAMPS = 4;
enum class NativeWindowScalingMode { ScaleToWindow, Freeze };
enum class NativeWindowTransform { Identity };
inline const char *ToString(NativeWindowScalingMode) { return "mode"; }
inline const char *ToString(NativeWindowTransform) { return "transform"; }
struct Rect {
    int left{}, right{}, top{}, bottom{};
    explicit operator bool() const { return false; }
};
struct ANativeWindow;
inline int Perform(ANativeWindow *, int, ...) { return 0; }
struct ANativeWindow {
    struct { int magic{AndroidNativeWindowMagic}, version{sizeof(ANativeWindow)}; } common;
    int (*perform)(ANativeWindow *, int, ...) = Perform;
};
// Linux Vulkan headers omit Android's platform declarations. Keep only that
// platform boundary as a facade; the remaining Vulkan structs/enums are real.
namespace vk { struct AndroidSurfaceCreateInfoKHR { ANativeWindow *window{}; }; }
struct Env {
    bool IsSameObject(jobject a, jobject b) { return a == b; }
    void DeleteGlobalRef(jobject) {}
    jobject NewGlobalRef(jobject value) { return value; }
};
inline ANativeWindow *ANativeWindow_fromSurface(Env *, jobject) { static ANativeWindow window; return &window; }
inline void ANativeWindow_release(ANativeWindow *) {}
struct Jvm { Env env; Env *GetEnv() { return &env; } };
namespace texture {
struct Format {
    vk::Format value{vk::Format::eR8G8B8A8Unorm};
    vk::Format operator*() const { return value; }
    explicit operator bool() const { return true; }
    bool operator==(const Format &) const = default;
};
struct Dimensions {
    u32 width{1920}, height{1080}, depth{1};
    operator vk::Extent2D() const { return vk::Extent2D{width, height}; }
    explicit operator bool() const { return width && height; }
};
}
namespace format { inline texture::Format R8G8B8A8Unorm; }
struct QueueState {
    std::promise<void> entered, release;
    std::shared_future<void> gate{release.get_future()};
    std::atomic<unsigned> idleCalls{}, destroyed{}, premature{};
    std::atomic<bool> pending{true};
    bool fail{};
};
inline thread_local bool ownsQueue{};
struct QueueMutex {
    std::mutex mutex;
    void lock() { mutex.lock(); ownsQueue = true; }
    void unlock() { ownsQueue = false; mutex.unlock(); }
};
struct Queue {
    QueueState &state;
    void waitIdle() {
        if (!ownsQueue) throw std::runtime_error("queue wait lacks external synchronization");
        ++state.idleCalls;
        state.entered.set_value();
        state.gate.wait();
        if (state.fail) throw std::runtime_error("device lost");
        state.pending = false;
    }
};
struct Device { QueueState &state; };
struct Surface {
    Surface(int, vk::AndroidSurfaceCreateInfoKHR) {}
    vk::SurfaceKHR operator*() const { return {}; }
};
struct Swapchain {
    QueueState &state;
    Swapchain(Device &device, vk::SwapchainCreateInfoKHR) : state{device.state} {}
    ~Swapchain() { ++state.destroyed; if (state.pending) ++state.premature; }
    std::vector<vk::Image> getImages() { return std::vector<vk::Image>(2); }
};
struct Physical {
    bool getSurfaceSupportKHR(unsigned, vk::SurfaceKHR) { return true; }
    vk::SurfaceCapabilitiesKHR getSurfaceCapabilitiesKHR(vk::SurfaceKHR) {
        vk::SurfaceCapabilitiesKHR result{};
        result.minImageCount = 2; result.maxImageCount = 3;
        result.maxImageExtent = vk::Extent2D{4096, 4096};
        result.supportedUsageFlags = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
        result.supportedCompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
        return result;
    }
    std::vector<vk::SurfaceFormatKHR> getSurfaceFormatsKHR(vk::SurfaceKHR) { return {{vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear}}; }
    std::vector<vk::PresentModeKHR> getSurfacePresentModesKHR(vk::SurfaceKHR) { return {vk::PresentModeKHR::eFifo}; }
};
struct GPU {
    Device vkDevice;
    Queue vkQueue;
    QueueMutex queueMutex;
    Physical vkPhysicalDevice;
    unsigned vkQueueFamilyIndex{};
    int vkInstance{};
    explicit GPU(QueueState &state) : vkDevice{state}, vkQueue{state} {}
};
struct Settings { bool no{}; bool *forceTripleBuffering{&no}, *disableFrameThrottling{&no}; };
struct DeviceState { GPU *gpu; Jvm *jvm; Settings *settings; };
struct Texture {
    template<class... T> explicit Texture(T &&...) {}
    void TransitionLayout(vk::ImageLayout) {}
};
class PresentationEngine {
  public:
    DeviceState &state;
    GPU &gpu;
    std::mutex mutex;
    std::condition_variable surfaceCondition;
    jobject jSurface{reinterpret_cast<void *>(1)};
    ANativeWindow *window{};
    Rect windowCrop;
    NativeWindowScalingMode windowScalingMode{NativeWindowScalingMode::ScaleToWindow};
    NativeWindowTransform windowTransform{NativeWindowTransform::Identity};
    std::optional<Surface> vkSurface;
    vk::SurfaceCapabilitiesKHR vkSurfaceCapabilities;
    std::optional<Swapchain> vkSwapchain;
    texture::Format swapchainFormat;
    texture::Dimensions swapchainExtent;
    static constexpr size_t MaxSwapchainImageCount = 10;
    std::array<std::shared_ptr<Texture>, MaxSwapchainImageCount> images;
    std::array<std::shared_ptr<int>, MaxSwapchainImageCount> frameFences;
    size_t frameIndex{}, swapchainImageCount{};
    bool stopping{}, swapchainNeedsRecreation{};
    explicit PresentationEngine(DeviceState &state) : state{state}, gpu{*state.gpu} {
        vkSurface.emplace(0, vk::AndroidSurfaceCreateInfoKHR{});
        vkSurfaceCapabilities = gpu.vkPhysicalDevice.getSurfaceCapabilitiesKHR({});
    }
    void DestroySwapchain();
    void UpdateSurface(jobject);
    void UpdateSwapchain(texture::Format, texture::Dimensions);
};
#include "production.inc"

static unsigned failures;
static void check(bool value, const char *message) {
    std::cout << (value ? "PASS " : "FAIL ") << message << '\n';
    failures += !value;
}
static void retirement(int mode, bool fail = false) {
    QueueState queue;
    GPU gpu{queue}; Jvm jvm; Settings settings;
    DeviceState state{&gpu, &jvm, &settings};
    PresentationEngine engine{state};
    engine.vkSwapchain.emplace(gpu.vkDevice, vk::SwapchainCreateInfoKHR{});
    engine.images[0] = std::make_shared<Texture>();
    engine.frameFences[0] = std::make_shared<int>(1);
    queue.fail = fail;
    auto entered = queue.entered.get_future();
    auto update = std::async(std::launch::async, [&] {
        try {
            if (mode == 2) {
                std::scoped_lock lock{engine.mutex};
                engine.UpdateSwapchain({}, {1280, 720, 1});
            } else engine.UpdateSurface(mode == 0 ? nullptr : reinterpret_cast<void *>(2));
            return false;
        } catch (const std::runtime_error &) { return true; }
    });
    bool waiting = entered.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    if (waiting) {
        check(update.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
              "surface update waits for pending GPU commands");
        check(queue.destroyed == 0, "old swapchain survives while GPU is pending");
    }
    queue.release.set_value();
    const bool threw = update.get();
    check(waiting && queue.idleCalls == 1, "detach/replace/resize drains the externally synchronized queue once");
    check(queue.premature == 0, "swapchain images are never destroyed during pending GPU writes");
    if (fail) {
        check(threw && engine.vkSwapchain && queue.destroyed == 0 && engine.images[0] && engine.frameFences[0]
              && engine.vkSurface && engine.jSurface == reinterpret_cast<void *>(1),
              "failed GPU wait preserves old swapchain, resources and surface state");
    } else {
        check(!threw && queue.destroyed == 1 && !engine.frameFences[0]
              && (mode != 0 || (!engine.images[0] && !engine.vkSwapchain && !engine.vkSurface && !engine.jSurface)),
              "completed commands permit retirement and release old lifecycle references");
    }
    queue.pending = false; // Test cleanup after negative controls or simulated failure.
}
static void firstSurface() {
    QueueState queue; queue.pending = false;
    GPU gpu{queue}; Jvm jvm; Settings settings;
    DeviceState state{&gpu, &jvm, &settings}; PresentationEngine engine{state};
    engine.UpdateSurface(reinterpret_cast<void *>(2));
    check(queue.idleCalls == 0 && engine.vkSwapchain.has_value(), "initial surface creation never stalls the GPU queue");
}
int main() {
    retirement(0); retirement(1); retirement(2);
    retirement(0, true); retirement(1, true); retirement(2, true); firstSurface();
    return failures ? 1 : 0;
}
