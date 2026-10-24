"""Extend the existing Vulkan harness only at its explicit Android host facades."""
def prepare(root, directory):
    common = root / 'app/src/main/cpp/skyline/common'
    def without_includes(path):
        return '\n'.join(line for line in path.read_text().splitlines()
                         if not line.startswith('#include') and line != '#pragma once')
    (directory / 'traps.inc').write_text(
        without_includes(common / 'interval_map.h') + '\n' +
        without_includes(common / 'trap_manager.h') + '\n' +
        without_includes(common / 'trap_manager.cpp'))
    base = (root / 'tests/gpu_backing_promotion/tests.cpp').read_text()
    start = base.index('            struct Memory {')
    end = base.index('        struct State {', start)
    base = base[:start] + (root / 'tests/gpu_overlap_authority/host_boundaries.inc').read_text() + '\n        };\n' + base[end:]
    base = base.replace('namespace skyline::gpu {', '#include "traps.inc"\nnamespace skyline::gpu {', 1)
    base = base.replace('int main() {', 'int unusedBackingPromotionMain() {')
    # Renamed main no longer has the language's implicit return.
    end = base.rfind('}')
    base = base[:end] + '    return 0;\n' + base[end:]
    base = base.replace('        void UpdateRenderPassUsage(', '        bool ValidateRenderPassUsage(u32, texture::RenderPassUsage);\n        void UpdateRenderPassUsage(')
    base = base.replace('    using std::span<T>::span;', '    using std::span<T>::span;\n    span(T &value) : std::span<T>(&value,1) {}')
    facade = r'''namespace ranges {
        using std::ranges::all_of; using std::ranges::transform;
        namespace views { template<class A,class B> auto concat(A a,B b) {
            std::vector<TextureView *> out(a.begin(),a.end()); out.insert(out.end(),b.begin(),b.end()); return out;
        } }
    }
    namespace node {
        struct RenderPassEndNode {};
        struct RenderPassNode {
            vk::Rect2D renderArea;
            template<class... A> void AddSubpass(A&&...) {}
            void UpdateDependency(vk::PipelineStageFlags,vk::PipelineStageFlags) {}
        };
    }
'''
    base = base.replace('    namespace interconnect {', '    namespace interconnect {\n'+facade,1)
    base = base.replace('            ContextTag tag{1};', r'''            int gpu{};
            using Nodes=std::list<std::variant<node::RenderPassNode,node::RenderPassEndNode>>;
            struct Slot { Nodes nodes,pendingPostRenderPassNodes; } localSlot;
            Slot *slot{&localSlot};
            node::RenderPassNode *renderPass{};
            Nodes::iterator renderPassIt;
            std::vector<vk::ImageView> lastSubpassColorAttachments,lastSubpassInputAttachments;
            vk::ImageView lastSubpassDepthStencilAttachment{};
            u32 renderPassIndex{},subpassCount{};
            std::vector<std::function<void()>> subpassBoundaryCallbacks;
            static bool ViewsEqual(vk::ImageView a,TextureView *b) { return a==(b ? b->GetView() : vk::ImageView{}); }
            bool CreateRenderPassWithSubpass(vk::Rect2D,span<TextureView *>,span<TextureView *>,span<TextureView *>,TextureView *,bool,vk::PipelineStageFlags,vk::PipelineStageFlags);
            ContextTag tag{1};''')
    base = base.replace('        #include "attach.inc"', '        #include "attach.inc"\n        #include "executor_pass.inc"')
    (directory / 'backing_harness.inc').write_text(base)
