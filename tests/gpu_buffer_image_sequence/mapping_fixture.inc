// Host channel boundary backed by the fixture's exact production GMMU bodies.
namespace skyline::soc::gm20b {
    using IOVA=u64;
    struct HostGmmu {
        translation_test::Gmmu source;
        std::function<void()> afterTranslation;
        HostGmmu() { source.blocks={{0,nullptr,{}}}; }
        void Map(IOVA address,u8 *physical,size_t size) { source.Map(address,physical,size); }
        void Unmap(IOVA address,size_t size) { source.Unmap(address,size); }
        void Copy(IOVA,IOVA,size_t) { throw std::runtime_error("legacy fragmented copy is outside this fixture"); }
        auto ReadMappingEpoch() const { return source.ReadMappingEpoch(); }
        auto MappingRevision() const { return source.MappingRevision(); }
        struct TrackedTranslation { std::vector<::span<u8>> mappings; MappingIdentity::Snapshot identity; bool fullyResident{}; };
        TrackedTranslation TranslateRangeTracked(IOVA address,size_t size) {
            auto result=source.TranslateRangeTracked(address,size);
            TrackedTranslation output{{},result.identity,result.fullyResident};
            for (auto mapping : result.mappings) output.mappings.emplace_back(mapping.data(),mapping.size());
            if (auto callback=std::exchange(afterTranslation,{})) callback();
            return output;
        }
        std::vector<::span<u8>> TranslateRange(IOVA address,size_t size) { return TranslateRangeTracked(address,size).mappings; }
    };
    struct AddressSpaceContext { HostGmmu gmmu; };
    struct ChannelContext {
        std::shared_ptr<AddressSpaceContext> asCtx{std::make_shared<AddressSpaceContext>()};
        skyline::gpu::interconnect::CommandExecutor &executor;
        explicit ChannelContext(skyline::gpu::interconnect::CommandExecutor &executor) : executor{executor} {}
    };
}
namespace skyline::gpu::interconnect {
    #include "dma_class.inc"
    #include "inline_class.inc"
}
#include "dma_entrypoints.inc"
#include "inline_entrypoints.inc"

namespace skyline::gpu::interconnect {
    struct InterconnectContext { GPU &gpu; soc::gm20b::ChannelContext &channelCtx; CommandExecutor &executor; };
    namespace maxwell3d {
        // Selector facade supplies an enrolled translated view; the complete
        // publication/read-modify-write Load body below is production code.
        struct ConstantBuffers {
            struct Selector {
                std::optional<BufferView> view;
                size_t requested{};
                Selector &UpdateGet(InterconnectContext &ctx,size_t size) {
                    requested=size;
                    auto &gmmu=ctx.channelCtx.asCtx->gmmu;
                    ctx.executor.StartTextureMapping(gmmu.ReadMappingEpoch());
                    auto mappings=gmmu.TranslateRange(0x10000,size);
                    view=ctx.gpu.buffer.FindOrCreate(mappings.front(),ctx.executor.tag,[&](std::shared_ptr<Buffer> buffer,ContextLock<Buffer> &&lock) { ctx.executor.AttachLockedBuffer(buffer,std::move(lock)); });
                    return *this;
                }
            } selectorState;
            void DisableQuickBind() {}
            void Load(InterconnectContext &,span<u32>,u32);
        };
        #include "cbuf_load.inc"
    }
}
