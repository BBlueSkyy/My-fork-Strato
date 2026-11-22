// SPDX-License-Identifier: MPL-2.0
// This file is appended after both extracted production implementations.
struct Fixture {
    std::array<before::Pipeline, 33> beforePipelines;
    std::array<after::Pipeline, 33> afterPipelines;
    explicit Fixture(unsigned queries, int mismatch) {
        const auto fill = [&](auto &pipelines) {
            using S = skyline::gpu::interconnect::PipelineSpecialization;
            for (auto &p : pipelines) {
                for (unsigned q = 0; q < queries; ++q) {
                    S::TextureSource source{0, q * 4, 0, 0, q * 4, 0};
                    p.specialization.RecordTexture(S::Kind::TextureType, q % 5, source, q);
                }
            }
            if (mismatch == 1)
                for (unsigned i = 1; i < pipelines.size(); ++i) pipelines[i].stageMask = 1;
            if (mismatch == 2)
                for (unsigned i = 1; i < pipelines.size(); ++i)
                    pipelines[i].specialization.Record(S::Kind::ConstantBuffer, 0, 0, 0, 1);
            if (mismatch == 3) {
                // Realistic cache miss after this source has seen other pairs.
                for (unsigned i = 1; i < 17; ++i)
                    pipelines[i].specialization.Record(S::Kind::ConstantBuffer, 0, 0, 0, 1);
                for (unsigned i = 17; i < pipelines.size(); ++i) pipelines[0].CheckBindingMatch(&pipelines[i]);
            }
        };
        fill(beforePipelines);
        fill(afterPipelines);
    }
};

template<bool After>
static double Run(Fixture &fixture, unsigned n, bool gate, bool enabled, bool cold) {
    auto &pipelines = [&]() -> auto & {
        if constexpr (After) return fixture.afterPipelines;
        else return fixture.beforePipelines;
    }();
    std::size_t sum{};
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < n; ++i) {
        auto *target = &pipelines[1 + i % 16];
        if (cold) pipelines[0].bindingMatchCache.clear();
        if (gate) {
            if constexpr (After) sum += after::Prepare(&pipelines[0], target, enabled) != nullptr;
            else sum += before::Prepare(&pipelines[0], target, enabled) != nullptr;
        } else {
            sum += pipelines[0].CheckBindingMatch(target);
        }
        asm volatile("" : : "g"(sum) : "memory");
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    return elapsed / n;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const unsigned iterations = std::stoul(argv[1]), rounds = std::stoul(argv[2]);
    struct Case { const char *name; unsigned queries; int mismatch; bool gate, enabled, cold; };
    const Case cases[]{
        {"cached_equal_empty", 0, 0, false, true, false},
        {"cached_equal_12", 12, 0, false, true, false},
        {"cached_equal_48", 48, 0, false, true, false},
        {"cached_false_48", 48, 1, false, true, false},
        {"different_specialization_48", 48, 2, false, true, false},
        {"different_specialization_populated_48", 48, 3, false, true, false},
        {"quick_bind_disabled_48", 48, 0, true, false, false},
        {"quick_bind_enabled_48", 48, 0, true, true, false},
        {"cold_equal_48_with_clear", 48, 0, false, true, true},
    };
    std::printf("[");
    bool first = true;
    for (const auto &c : cases) {
        Fixture fixture(c.queries, c.mismatch);
        Run<false>(fixture, 10000, c.gate, c.enabled, c.cold);
        Run<true>(fixture, 10000, c.gate, c.enabled, c.cold);
        std::vector<double> oldTimes, newTimes;
        for (unsigned r = 0; r < rounds; ++r) {
            if (r & 1) {
                newTimes.push_back(Run<true>(fixture, iterations, c.gate, c.enabled, c.cold));
                oldTimes.push_back(Run<false>(fixture, iterations, c.gate, c.enabled, c.cold));
            } else {
                oldTimes.push_back(Run<false>(fixture, iterations, c.gate, c.enabled, c.cold));
                newTimes.push_back(Run<true>(fixture, iterations, c.gate, c.enabled, c.cold));
            }
        }
        // Reordering must never add retained cache entries.
        if (fixture.afterPipelines[0].bindingMatchCache.size() > fixture.beforePipelines[0].bindingMatchCache.size())
            throw std::runtime_error("cache retention increased");
        std::printf("%s{\"case\":\"%s\",\"before_ns\":[", first ? "" : ",", c.name);
        first = false;
        for (unsigned r = 0; r < rounds; ++r) std::printf("%s%.3f", r ? "," : "", oldTimes[r]);
        std::printf("],\"after_ns\":[");
        for (unsigned r = 0; r < rounds; ++r) std::printf("%s%.3f", r ? "," : "", newTimes[r]);
        std::printf("]}");
    }
    std::printf("]\n");
}
