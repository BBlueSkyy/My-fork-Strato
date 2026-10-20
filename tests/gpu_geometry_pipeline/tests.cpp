#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <shader-compiler/runtime_info.h>
#include <shader_compiler/stage.h>
namespace ranges = std::ranges;
namespace Shader {
enum class OutputTopology { PointList, LineStrip, TriangleStrip };
namespace IR {
struct Program {
    struct {
        Shader::VaryingState stores, passthrough;
        std::map<Shader::IR::Attribute, Shader::IR::Attribute> legacy_stores_mapping;
    } info;
    Shader::Stage stage{};
    Shader::OutputTopology output_topology{};
    bool is_geometry_passthrough{};
};
}
}
namespace engine {
#include "topology.inc"
struct TessellationParameters { enum class OutputPrimitives { TrianglesCCW }; };
}
struct PackedPipelineState {
    engine::DrawTopology topology{engine::DrawTopology::Triangles};
    std::array<int, 32> vertexAttributes{};
    float pointSize{3};
    bool transformFeedbackEnable{}, openGlNdc{}, alphaTestEnable{}, apiMandatedEarlyZ{}, flipYEnable{};
    float alphaRef{};
    engine::TessellationParameters::OutputPrimitives outputPrimitives{};
    int domainType{}, spacing{};
    std::vector<Shader::TransformFeedbackVarying> GetTransformFeedbackVaryings() const {
        return {{0, 16, 0, 4}};
    }
    Shader::CompareFunction GetAlphaFunc() const { return Shader::CompareFunction::Less; }
};
static Shader::AttributeType ConvertShaderAttributeType(int) { return Shader::AttributeType::Float; }
static Shader::TessPrimitive ConvertShaderTessPrimitive(int) { return Shader::TessPrimitive::Triangles; }
static Shader::TessSpacing ConvertShaderTessSpacing(int) { return Shader::TessSpacing::Equal; }
#include "production.inc"
static void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    Require(!HasConventionalGeometryStage(false, false, false), "no geometry stage");
    Require(HasConventionalGeometryStage(true, false, false), "ordinary and lowered guest GS stages");
    Require(HasConventionalGeometryStage(false, true, false), "synthetic Layer GS is a physical geometry stage");
    Require(!HasConventionalGeometryStage(true, false, true), "native passthrough leaves final conversion upstream");
    PackedPipelineState state;
    state.openGlNdc = true;
    state.transformFeedbackEnable = true;
    Shader::IR::Program vertex, geometry;
    vertex.stage = Shader::Stage::VertexB;
    geometry.stage = Shader::Stage::Geometry;
    geometry.is_geometry_passthrough = true;
    auto runtime = MakeRuntimeInfo(state, geometry, &vertex, false);
    Require(!runtime.convert_depth_mode, "native passthrough must not convert position a second time");
    Require(runtime.xfb_varyings.empty(), "native passthrough must not redeclare predecessor transform feedback");
    runtime = MakeRuntimeInfo(state, vertex, nullptr, false);
    Require(runtime.convert_depth_mode, "vertex owns final conversion without a conventional GS");
    geometry.is_geometry_passthrough = false;
    runtime = MakeRuntimeInfo(state, geometry, &vertex, true);
    Require(runtime.convert_depth_mode, "conventional GS owns final conversion");
    Require(runtime.xfb_varyings.size() == 1, "conventional GS owns transform feedback");
    runtime = MakeRuntimeInfo(state, vertex, nullptr, true);
    Require(!runtime.convert_depth_mode, "vertex must leave position unchanged before conventional GS");
    Require(runtime.xfb_varyings.empty(), "vertex must not capture before conventional GS");
    for (auto topology : {engine::DrawTopology::Lines, engine::DrawTopology::LineLoop,
                          engine::DrawTopology::LineStrip, engine::DrawTopology::LineListAdjcy,
                          engine::DrawTopology::LineStripAdjcy}) {
        Require(ConvertShaderOutputTopology(topology) == Shader::OutputTopology::LineStrip,
                "synthetic Layer GS must use a line output for line input");
    }
    Require(ConvertShaderOutputTopology(engine::DrawTopology::Points) == Shader::OutputTopology::PointList,
            "synthetic point topology");
    Require(ConvertShaderOutputTopology(engine::DrawTopology::Triangles) == Shader::OutputTopology::TriangleStrip,
            "synthetic triangle topology");
    geometry.info.stores.Set(Shader::IR::Attribute::Generic0X);
    geometry.info.passthrough.Set(Shader::IR::Attribute::Generic1X);
    geometry.info.legacy_stores_mapping.emplace(Shader::IR::Attribute::ColorFrontDiffuseR,
                                               Shader::IR::Attribute::Generic2X);
    geometry.is_geometry_passthrough = true;
    Shader::IR::Program fragment;
    fragment.stage = Shader::Stage::Fragment;
    runtime = MakeRuntimeInfo(state, fragment, &geometry, false);
    Require(runtime.previous_stage_stores[Shader::IR::Attribute::Generic0X], "explicit GS store metadata");
    Require(runtime.previous_stage_stores[Shader::IR::Attribute::Generic1X], "native GS passthrough metadata");
    Require(runtime.previous_stage_legacy_stores_mapping.at(Shader::IR::Attribute::ColorFrontDiffuseR) ==
            Shader::IR::Attribute::Generic2X, "preserve predecessor legacy varying allocation");
    state.topology = engine::DrawTopology::Points;
    geometry.output_topology = Shader::OutputTopology::PointList;
    runtime = MakeRuntimeInfo(state, geometry, &vertex, false);
    Require(!runtime.fixed_state_point_size, "native passthrough keeps predecessor point size");
    geometry.is_geometry_passthrough = false;
    runtime = MakeRuntimeInfo(state, geometry, &vertex, true);
    Require(runtime.fixed_state_point_size == state.pointSize, "conventional point GS fixed size");
    std::cout << "geometry pipeline runtime regressions passed\n";
}
