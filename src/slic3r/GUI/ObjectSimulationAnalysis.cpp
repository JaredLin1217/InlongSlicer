#include "ObjectSimulationAnalysis.hpp"

#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/PathVertex.hpp>
#include <libvgcode/include/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace {

static float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static float smoothstep01(float value)
{
    const float t = clamp01(value);
    return t * t * (3.0f - 2.0f * t);
}

static float distance_2d(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

static float segment_length_xy(const libvgcode::PathVertex& a, const libvgcode::PathVertex& b)
{
    return distance_2d(a.position[0], a.position[1], b.position[0], b.position[1]);
}

static bool is_excluded_role(libvgcode::EGCodeExtrusionRole role)
{
    using Role = libvgcode::EGCodeExtrusionRole;
    return role == Role::None ||
        role == Role::SupportMaterial ||
        role == Role::SupportMaterialInterface ||
        role == Role::SupportTransition ||
        role == Role::BridgeInfill ||
        role == Role::InternalBridgeInfill ||
        role == Role::WipeTower ||
        role == Role::Skirt ||
        role == Role::Brim ||
        role == Role::Custom;
}

static bool is_object_extrusion(const libvgcode::PathVertex& vertex)
{
    return vertex.is_extrusion() && !is_excluded_role(vertex.role);
}

static bool is_solid_role(libvgcode::EGCodeExtrusionRole role)
{
    using Role = libvgcode::EGCodeExtrusionRole;
    return role == Role::BottomSurface ||
        role == Role::SolidInfill ||
        role == Role::TopSolidInfill;
}

static bool is_model_infill_role(libvgcode::EGCodeExtrusionRole role)
{
    return role == libvgcode::EGCodeExtrusionRole::InternalInfill;
}

static bool is_gap_role(libvgcode::EGCodeExtrusionRole role)
{
    return role == libvgcode::EGCodeExtrusionRole::GapFill;
}

static bool is_wall_role(libvgcode::EGCodeExtrusionRole role)
{
    using Role = libvgcode::EGCodeExtrusionRole;
    return role == Role::ExternalPerimeter ||
        role == Role::OverhangPerimeter ||
        role == Role::Perimeter;
}

static int wall_role_group(libvgcode::EGCodeExtrusionRole role)
{
    using Role = libvgcode::EGCodeExtrusionRole;
    if (role == Role::ExternalPerimeter || role == Role::OverhangPerimeter)
        return 1;
    if (role == Role::Perimeter)
        return 2;
    return 0;
}

struct SegmentGeometry
{
    bool valid{ false };
    float mid_x{ 0.0f };
    float mid_y{ 0.0f };
    float length{ 0.0f };
};

static bool previous_position_is_usable(const libvgcode::PathVertex& prev, const libvgcode::PathVertex& vertex)
{
    return prev.layer_id == vertex.layer_id &&
        prev.position[0] != std::numeric_limits<float>::max() &&
        prev.position[1] != std::numeric_limits<float>::max() &&
        std::fabs(prev.position[2] - vertex.position[2]) < 1.0f;
}

static SegmentGeometry segment_geometry(const libvgcode::GCodeInputData& data, size_t vertex_idx)
{
    SegmentGeometry out;
    if (vertex_idx >= data.vertices.size())
        return out;

    const libvgcode::PathVertex& vertex = data.vertices[vertex_idx];
    if (!is_object_extrusion(vertex))
        return out;

    const libvgcode::PathVertex* prev = nullptr;
    if (vertex_idx > 0 && previous_position_is_usable(data.vertices[vertex_idx - 1], vertex))
        prev = &data.vertices[vertex_idx - 1];

    const float x0 = prev != nullptr ? prev->position[0] : vertex.position[0];
    const float y0 = prev != nullptr ? prev->position[1] : vertex.position[1];
    const float x1 = vertex.position[0];
    const float y1 = vertex.position[1];
    const float length = distance_2d(x0, y0, x1, y1);
    if (length < 0.01f)
        return out;

    out.valid = true;
    out.mid_x = 0.5f * (x0 + x1);
    out.mid_y = 0.5f * (y0 + y1);
    out.length = length;
    return out;
}

struct LayerField
{
    bool valid{ false };
    float min_x{ std::numeric_limits<float>::max() };
    float min_y{ std::numeric_limits<float>::max() };
    float max_x{ -std::numeric_limits<float>::max() };
    float max_y{ -std::numeric_limits<float>::max() };
    float object_length{ 0.0f };
    float solid_length{ 0.0f };
    float infill_length{ 0.0f };
    float gap_length{ 0.0f };
    float wall_length{ 0.0f };
    int object_count{ 0 };
    int solid_count{ 0 };
    int infill_count{ 0 };
    int gap_count{ 0 };
    int wall_count{ 0 };

    void expand(float x, float y)
    {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        valid = true;
    }

    void add_role(libvgcode::EGCodeExtrusionRole role, float length)
    {
        object_length += length;
        ++object_count;
        if (is_solid_role(role)) {
            solid_length += length;
            ++solid_count;
        } else if (is_model_infill_role(role)) {
            infill_length += length;
            ++infill_count;
        } else if (is_gap_role(role)) {
            gap_length += length;
            ++gap_count;
        } else if (is_wall_role(role)) {
            wall_length += length;
            ++wall_count;
        }
    }

    float width() const { return std::max(0.1f, max_x - min_x); }
    float depth() const { return std::max(0.1f, max_y - min_y); }
    float area() const { return width() * depth(); }
};

static float local_corner_influence(const libvgcode::GCodeInputData& data, size_t vertex_idx)
{
    if (vertex_idx == 0 || vertex_idx + 1 >= data.vertices.size())
        return 0.0f;

    const libvgcode::PathVertex& prev = data.vertices[vertex_idx - 1];
    const libvgcode::PathVertex& vertex = data.vertices[vertex_idx];
    const libvgcode::PathVertex& next = data.vertices[vertex_idx + 1];
    if (!prev.is_extrusion() || !vertex.is_extrusion() || !next.is_extrusion())
        return 0.0f;
    if (!is_wall_role(prev.role) || !is_wall_role(vertex.role) || !is_wall_role(next.role))
        return 0.0f;
    if (prev.layer_id != vertex.layer_id || next.layer_id != vertex.layer_id)
        return 0.0f;
    if (wall_role_group(prev.role) != wall_role_group(vertex.role) ||
        wall_role_group(next.role) != wall_role_group(vertex.role))
        return 0.0f;

    const float al = segment_length_xy(prev, vertex);
    const float bl = segment_length_xy(vertex, next);
    if (al < 0.5f || bl < 0.5f)
        return 0.0f;

    const float ax = vertex.position[0] - prev.position[0];
    const float ay = vertex.position[1] - prev.position[1];
    const float bx = next.position[0] - vertex.position[0];
    const float by = next.position[1] - vertex.position[1];
    const float cos_angle = std::clamp((ax * bx + ay * by) / (al * bl), -1.0f, 1.0f);
    return smoothstep01((0.94f - cos_angle) / 0.42f);
}

static float retained_heat_from_previous_layers(const std::unordered_map<uint32_t, LayerField>& layers,
                                                uint32_t layer_id,
                                                WarpThermalResolution resolution)
{
    if (layer_id == 0)
        return 0.0f;

    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    const ObjectSimulationResolutionProfile shape = object_simulation_resolution_profile(resolution);
    for (int offset = 1; offset <= shape.retained_layers; ++offset) {
        if (layer_id < static_cast<uint32_t>(offset))
            break;
        const auto it = layers.find(layer_id - static_cast<uint32_t>(offset));
        if (it == layers.end() || !it->second.valid)
            continue;
        const float decay = object_simulation_retained_heat_decay(offset, resolution);
        weighted_sum += decay * object_simulation_layer_coverage_heat(
            it->second.object_length,
            it->second.solid_length,
            it->second.infill_length,
            it->second.gap_length,
            it->second.wall_length,
            it->second.area());
        weight_sum += decay;
    }
    return weight_sum > 0.0f ? clamp01(weighted_sum / weight_sum) : 0.0f;
}

static float layer_large_area_factor(const LayerField& layer)
{
    if (!layer.valid)
        return 0.0f;
    return smoothstep01((layer.area() - 1600.0f) / 16000.0f);
}

static ObjectSimulationRole simulation_role_for(libvgcode::EGCodeExtrusionRole role, float corner)
{
    if (is_gap_role(role))
        return ObjectSimulationRole::Gap;
    if (is_model_infill_role(role))
        return ObjectSimulationRole::Infill;
    if (is_solid_role(role))
        return ObjectSimulationRole::Solid;
    if (is_wall_role(role)) {
        if (corner > 0.18f)
            return ObjectSimulationRole::Corner;
        return wall_role_group(role) == 1 ? ObjectSimulationRole::OuterWall : ObjectSimulationRole::InnerWall;
    }
    return ObjectSimulationRole::Other;
}

} // namespace

void assign_object_simulation_display_risk(libvgcode::GCodeInputData& data, const ObjectSimulationConfig& config)
{
    std::unordered_map<uint32_t, LayerField> layers;
    std::vector<SegmentGeometry> geometries(data.vertices.size());

    for (size_t i = 0; i < data.vertices.size(); ++i) {
        libvgcode::PathVertex& vertex = data.vertices[i];
        if (!is_object_extrusion(vertex))
            continue;

        SegmentGeometry geometry = segment_geometry(data, i);
        geometries[i] = geometry;
        LayerField& layer = layers[vertex.layer_id];
        layer.expand(vertex.position[0], vertex.position[1]);
        if (geometry.valid)
            layer.expand(geometry.mid_x, geometry.mid_y);
        layer.add_role(vertex.role, geometry.valid ? geometry.length : 0.0f);
    }

    for (size_t i = 0; i < data.vertices.size(); ++i) {
        libvgcode::PathVertex& vertex = data.vertices[i];
        if (!is_object_extrusion(vertex)) {
            vertex.object_simulation = 0.0f;
            continue;
        }

        const auto layer_it = layers.find(vertex.layer_id);
        if (layer_it == layers.end() || !layer_it->second.valid)
            continue;

        const LayerField& layer = layer_it->second;
        const SegmentGeometry& geometry = geometries[i];
        const float x = geometry.valid ? geometry.mid_x : vertex.position[0];
        const float y = geometry.valid ? geometry.mid_y : vertex.position[1];
        const float retained_heat = retained_heat_from_previous_layers(layers, vertex.layer_id, config.thermal_resolution);
        const float turn_corner = is_wall_role(vertex.role) ? local_corner_influence(data, i) : 0.0f;
        const ObjectSimulationRole role = simulation_role_for(vertex.role, turn_corner);
        if (role == ObjectSimulationRole::Other) {
            vertex.object_simulation = 0.0f;
            continue;
        }

        const float area = (is_solid_role(vertex.role) || is_model_infill_role(vertex.role) || is_gap_role(vertex.role)) ?
            layer_large_area_factor(layer) : 0.30f * layer_large_area_factor(layer);
        ObjectSimulationPathInput input;
        input.layer_id = vertex.layer_id;
        input.max_layers = std::max(1, config.analysis_layers);
        input.role = role;
        input.resolution = config.thermal_resolution;
        input.material_sensitivity = config.material_sensitivity;
        input.high_warp_material = config.high_warp_material;
        input.has_brim = config.has_brim;
        input.has_raft = config.has_raft;
        input.environment_influence = config.environment_influence;
        input.retained_heat = retained_heat;
        input.x = x;
        input.y = y;
        input.segment_length = geometry.valid ? geometry.length : 0.0f;
        input.corner_influence = turn_corner;
        input.layer_bounds_valid = layer.valid;
        input.layer_min_x = layer.min_x;
        input.layer_min_y = layer.min_y;
        input.layer_max_x = layer.max_x;
        input.layer_max_y = layer.max_y;
        input.layer_area_factor = area;

        const ObjectSimulationFieldSample sample = Slic3r::sample_object_simulation_path(input);
        vertex.object_simulation = clamp01(sample.display_risk);
    }
}

} // namespace GUI
} // namespace Slic3r
