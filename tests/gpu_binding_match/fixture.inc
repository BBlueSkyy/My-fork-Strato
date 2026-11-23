// Host facade: actual specialization and robin_map, scalar descriptor interface.
using skyline::gpu::interconnect::PipelineSpecialization;
struct DescriptorUpdateInfo {};
struct Context {};
inline DescriptorUpdateInfo result;
struct Pipeline {
    PipelineSpecialization specialization;
    int stageMask{}, descriptorInfo{};
    struct { bool usesPushDescriptors{}; } compiledPipeline;
    tsl::robin_map<Pipeline *, bool> bindingMatchCache;
    bool CheckBindingMatch(Pipeline *other);
    void InheritDescriptorBufferViews(const Pipeline &) {}
    DescriptorUpdateInfo *SyncDescriptors(Context &, int &, int &, int &, int &, int &, int &) { return &result; }
    DescriptorUpdateInfo *SyncDescriptorsQuickBind(Context &, int &, int &, int &, bool, int &, int &, int &) { return &result; }
};
