// SPDX-License-Identifier: MPL-2.0
// Exact production CopyToGuest and tiling helpers; no Vulkan I/O is involved.
#include "host_types.h"
#include <atomic>
#include <chrono>
#include <sys/mman.h>
#include <unistd.h>
namespace skyline {
    template<class T> using span = std::span<T>;
    namespace constant { constexpr size_t PageSize{4096}; }
    namespace util {
        template<class T> T *AlignUp(T *pointer, size_t alignment) { return reinterpret_cast<T *>(AlignUp(reinterpret_cast<uintptr_t>(pointer), alignment)); }
        template<class T> T *AlignDown(T *pointer, size_t alignment) { return reinterpret_cast<T *>(AlignDown(reinterpret_cast<uintptr_t>(pointer), alignment)); }
    }
}
namespace skyline::gpu {
    struct GPU {
        struct Memory {
            bool FreeHostMemory(std::span<u8> memory);
        };
        struct Process { Memory memory; };
        struct Settings {
            std::shared_ptr<bool> freeGuestTextureMemory{std::make_shared<bool>(true)};
            std::shared_ptr<bool> enableFastGpuReadbackHack{std::make_shared<bool>(false)};
        };
        struct {
            std::shared_ptr<Process> process{std::make_shared<Process>()};
            std::shared_ptr<Settings> settings{std::make_shared<Settings>()};
        } state;
    };
    class Texture {
      public:
        std::optional<GuestTexture> guest;
        std::span<u8> mirror;
        u32 levelCount, layerCount;
        size_t layerStride;
        std::vector<texture::MipLevelLayout> mipLayouts;
        vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
        GPU gpu{};
        const texture::Format *format{};
        texture::Dimensions dimensions{};
        std::atomic<bool> aliasReadOnly{};
        size_t guestMemoryPins{};
        bool memoryFreed{};
        std::chrono::nanoseconds accumulatedGuestWaitTime{};
        static constexpr std::chrono::nanoseconds SkipReadbackHackWaitTimeThreshold{250};
        void CopyToGuest(u8 *hostBuffer);
        void FreeGuest();
    };
#include "copy-to-guest.inc"
#include "free-guest.inc"
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#include "free-host-memory.inc"
}
namespace {
    using namespace skyline;
    using namespace skyline::gpu;
    void Check(bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    }
    // Independent byte addressing within one Maxwell GOB (64 bytes x 8 rows).
    size_t GobOffset(size_t x, size_t y) {
        return ((x & 32) << 3) + ((y & 6) << 5) + ((x & 16) << 1) +
               ((y & 1) << 4) + (x & 15);
    }
    void CopyCase(texture::TileMode mode, u32 layers, bool mipmapped = false) {
        constexpr size_t Guard{16};
        constexpr u8 GuestSentinel{0xE7}, SourceSentinel{0xC5};
        const texture::Format rgba{1, 1, 4};
        const bool block{mode == texture::TileMode::Block};
        const u32 width{block ? 8U : 3U}, height{mipmapped ? 8U : 2U};
        const size_t packedLayer{size_t{width} * height * 4};
        const u32 guestStride{block ? 1024U : mode == texture::TileMode::Pitch ? 48U : 32U};
        GuestTexture guest{
            .dimensions = {width, height, 1}, .format = &rgba,
            .tileConfig = {.mode = mode, .blockHeight = 1, .blockDepth = 1, .tileWidthSpacing = 0},
            .viewType = vk::ImageViewType::e2DArray, .layerStride = guestStride,
            .layerCount = layers, .mipLevelCount = mipmapped ? 2U : 1U,
        };
        if (mode == texture::TileMode::Pitch) guest.tileConfig.pitch = 20;
        const size_t sourceSize{(packedLayer + (mipmapped ? 64 : 0)) * layers};
        std::vector<u8> source(Guard + sourceSize + Guard, SourceSentinel);
        for (size_t i{}; i < sourceSize; ++i) source[Guard + i] = static_cast<u8>(1 + i % 191);
        const auto sourceBefore{source};
        std::vector<u8> destination(Guard + size_t{guestStride} * layers + Guard, GuestSentinel);
        auto expected{destination};
        for (u32 layer{}; layer < layers; ++layer) {
            size_t mipSourceBase{}, mipGuestBase{};
            for (u32 mip{}; mip < guest.mipLevelCount; ++mip) {
                const size_t rowBytes{(width >> mip) * 4}, rows{height >> mip};
                for (size_t y{}; y < rows; ++y) for (size_t x{}; x < rowBytes; ++x) {
                    const size_t offset{block ? GobOffset(x, y) :
                        mode == texture::TileMode::Pitch ? y * 20 + x : y * rowBytes + x};
                    expected[Guard + layer * guestStride + mipGuestBase + offset] =
                        sourceBefore[Guard + mipSourceBase + layer * rowBytes * rows + y * rowBytes + x];
                }
                mipSourceBase += rowBytes * rows * layers;
                mipGuestBase += 512;
            }
        }
        Texture texture{.guest = guest,
            .mirror = {destination.data() + Guard, size_t{guestStride} * layers},
            .levelCount = guest.mipLevelCount, .layerCount = layers, .layerStride = packedLayer,
            .mipLayouts = texture::GetBlockLinearMipLayout(guest.dimensions,
                1, 1, 4, 1, 1, 4, 1, 1, guest.mipLevelCount, 0),
        };
        texture.CopyToGuest(source.data() + Guard);
        Check(destination == expected, "CopyToGuest did not write GPU bytes to the correct guest layer/row or changed padding/guards");
        Check(source == sourceBefore, "CopyToGuest modified the GPU/staging source");
    }
    // A real shared-memory discard makes lost padding observable, then the exact
    // production CopyToGuest must reconstruct every discarded byte.
    void DiscardRoundTrip(texture::TileMode mode, bool rowPadding, bool layerPadding, u32 levels = 1,
                          vk::Format backingFormat = vk::Format::eR8G8B8A8Unorm, bool selfAliased = false) {
        constexpr u8 Sentinel{0xE7}, GpuByte{0x2B};
        const texture::Format rgba{1, 1, 4, backingFormat == vk::Format::eR8G8B8A8Unorm ? vk::ImageAspectFlagBits::eColor : vk::ImageAspectFlagBits::eDepth, backingFormat};
        const u32 width{rowPadding ? 8U : levels > 1 ? 64U : 16U}, height{64};
        GuestTexture guest{
            .dimensions = {width, height, 1}, .format = &rgba,
            .tileConfig = {.mode = mode, .blockHeight = 1, .blockDepth = 1, .tileWidthSpacing = 0},
            .viewType = vk::ImageViewType::e2DArray, .layerCount = 2, .mipLevelCount = levels,
        };
        if (mode == texture::TileMode::Pitch) guest.tileConfig.pitch = rowPadding ? 64 : width * 4;
        const auto layouts{texture::GetBlockLinearMipLayout(guest.dimensions, 1, 1, 4, 1, 1, 4, 1, 1, levels, 0)};
        size_t packed{}, tiled{};
        for (const auto &level : layouts) { packed += level.linearSize; tiled += level.blockLinearSize; }
        const size_t naturalStride{mode == texture::TileMode::Block ? tiled :
            mode == texture::TileMode::Pitch ? guest.tileConfig.pitch * height : width * height * 4};
        guest.layerStride = naturalStride + (layerPadding ? 4096 : 0);
        const size_t size{size_t{guest.layerStride} * guest.layerCount};
        int fd{memfd_create("texture-discard-test", 0)};
        Check(fd >= 0 && ftruncate(fd, size) == 0, "cannot create shared texture mapping");
        auto destination{static_cast<u8 *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))};
        close(fd);
        Check(destination != MAP_FAILED, "cannot map shared texture memory");
        std::memset(destination, Sentinel, size);
        std::vector<u8> source(packed * guest.layerCount, GpuByte);
        Texture texture{.guest = guest, .mirror = {destination, size}, .levelCount = levels,
            .layerCount = guest.layerCount, .layerStride = size_t{width} * height * 4,
            .mipLayouts = layouts, .format = &rgba, .dimensions = guest.dimensions};
        const bool padded{rowPadding || layerPadding || (mode == texture::TileMode::Block && packed != tiled)};
        const bool unsupportedDepth{backingFormat != vk::Format::eR8G8B8A8Unorm && backingFormat != vk::Format::eD32Sfloat};
        if (selfAliased) texture.guest->mappings = {{destination, guest.layerStride}, {destination, guest.layerStride}};
        texture.FreeGuest();
        texture.CopyToGuest(source.data());
        Check(destination[0] == GpuByte, "discard/recovery lost visible GPU data");
        if (layerPadding) Check(destination[naturalStride] == Sentinel, "discard/recovery destroyed layer padding");
        if (rowPadding && mode == texture::TileMode::Pitch)
            Check(destination[32] == Sentinel, "discard/recovery destroyed pitch row padding");
        if (rowPadding && mode == texture::TileMode::Block)
            Check(destination[256] == Sentinel, "discard/recovery destroyed block-linear GOB padding");
        if (levels == 4) Check(destination[21504 + 256] == Sentinel, "discard/recovery destroyed mip padding");
        if (!padded) Check(std::all_of(destination, destination + size, [](u8 byte) { return byte == GpuByte; }), "discard/recovery did not restore all visible bytes");
        Check(texture.memoryFreed == !(padded || unsupportedDepth || selfAliased), "discard eligibility did not match reversible footprint");
        munmap(destination, size);
    }

}
int main() {
    try {
        for (auto mode : {texture::TileMode::Linear, texture::TileMode::Pitch, texture::TileMode::Block})
            for (u32 layers : {1U, 3U}) CopyCase(mode, layers);
        CopyCase(texture::TileMode::Block, 3, true);
        for (auto mode : {texture::TileMode::Linear, texture::TileMode::Pitch, texture::TileMode::Block}) {
            DiscardRoundTrip(mode, false, false);
            DiscardRoundTrip(mode, false, true);
            if (mode != texture::TileMode::Linear) DiscardRoundTrip(mode, true, false);
        }
        DiscardRoundTrip(texture::TileMode::Block, false, false, 3);
        DiscardRoundTrip(texture::TileMode::Block, false, false, 4);
        for (auto format : {vk::Format::eD32Sfloat, vk::Format::eX8D24UnormPack32, vk::Format::eD24UnormS8Uint, vk::Format::eS8Uint})
            DiscardRoundTrip(texture::TileMode::Block, false, false, 1, format);
        DiscardRoundTrip(texture::TileMode::Block, false, false, 1, vk::Format::eR8G8B8A8Unorm, true);
        std::cout << "PASS: production CopyToGuest Linear/Pitch/Block, 1/3 layers, block mips, source immutability guest padding/guards and real discard/recovery eligibility\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
