#ifndef slic3r_OrganicSupportLayerPlan_hpp_
#define slic3r_OrganicSupportLayerPlan_hpp_

#include "TreeSupportCommon.hpp"
#include "SupportLayer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace TreeSupport3D {

struct OrganicSupportLayerPlanEntry
{
    coordf_t  print_z{0.};
    coordf_t  bottom_z{0.};
    coordf_t  height{0.};
    LayerIndex source_layer_begin{0};
    LayerIndex source_layer_end{0};
};

// Branch topology stays on the object-synchronized grid. This plan describes
// the independent Z grid used only when slicing the finished branch meshes.
class OrganicSupportLayerPlan
{
public:
    static OrganicSupportLayerPlan legacy(
        const SlicingParameters &slicing_params,
        const TreeSupportSettings &config,
        size_t source_layer_count)
    {
        OrganicSupportLayerPlan result;
        result.m_entries.reserve(source_layer_count);
        for (size_t layer_idx = 0; layer_idx < source_layer_count; ++layer_idx) {
            const coordf_t print_z  = layer_z(slicing_params, config, layer_idx);
            const coordf_t bottom_z = layer_idx == 0 ?
                (print_z < 0. ? print_z - slicing_params.layer_height : 0.) :
                layer_z(slicing_params, config, layer_idx - 1);
            result.m_entries.push_back({print_z, bottom_z, print_z - bottom_z,
                                        LayerIndex(layer_idx), LayerIndex(layer_idx)});
        }
        return result;
    }

    static std::optional<OrganicSupportLayerPlan> independent(
        const SlicingParameters              &slicing_params,
        const TreeSupportSettings            &config,
        size_t                                source_layer_count,
        const SupportGeneratorLayersPtr      &top_contacts,
        const std::vector<LayerIndex>         &mandatory_source_layers,
        bool                                  preserve_source_grid,
        std::string                           &failure_reason,
        const std::vector<coordf_t>           &bottom_clearance_planes = {})
    {
        if (source_layer_count == 0 || slicing_params.max_suport_layer_height <= EPSILON) {
            failure_reason = "missing source layers or maximum support layer height";
            return std::nullopt;
        }

        const coordf_t min_height = std::max<coordf_t>(slicing_params.min_layer_height, EPSILON);
        const coordf_t max_height = slicing_params.max_suport_layer_height;
        if (!std::isfinite(min_height) || !std::isfinite(max_height) || min_height > max_height + EPSILON) {
            failure_reason = "invalid minimum or maximum support layer height";
            return std::nullopt;
        }

        OrganicSupportLayerPlan result;
        result.m_independent = true;

        struct Anchor {
            coordf_t z;
            bool     required;
        };

        const size_t raft_count = std::min(config.raft_layers.size(), source_layer_count);
        std::vector<Anchor> anchors;
        anchors.reserve((preserve_source_grid ? source_layer_count : mandatory_source_layers.size() + 2) +
                        top_contacts.size() + raft_count + bottom_clearance_planes.size());
        for (size_t layer_idx = 0; layer_idx < raft_count; ++layer_idx)
            anchors.push_back({layer_z(slicing_params, config, layer_idx), true});

        if (raft_count == 0) {
            const coordf_t first_source_z = layer_z(slicing_params, config, 0);
            coordf_t first_contact_z = std::numeric_limits<coordf_t>::infinity();
            for (const SupportGeneratorLayer *contact : top_contacts)
                if (contact != nullptr && !contact->polygons.empty() && contact->print_z > EPSILON)
                    first_contact_z = std::min(first_contact_z, contact->print_z);
            if (first_contact_z > first_source_z + EPSILON) {
                // Never discard bed support to make room for a nearby exact
                // contact. Lower only the independent support first layer so
                // both layers remain printable, leaving the object grid and
                // the requested contact Z untouched. Raft heights stay fixed.
                const coordf_t bed_z = std::min(first_source_z, first_contact_z - min_height);
                if (bed_z < first_source_z - EPSILON && bed_z < min_height - EPSILON) {
                    failure_reason = "bed support and the first exact contact cannot satisfy the minimum support layer height";
                    return std::nullopt;
                }
                anchors.push_back({bed_z, true});
            }
        }

        if (preserve_source_grid) {
            for (size_t layer_idx = raft_count; layer_idx < source_layer_count; ++layer_idx)
                anchors.push_back({layer_z(slicing_params, config, layer_idx), false});
        } else if (source_layer_count > raft_count) {
            anchors.push_back({layer_z(slicing_params, config, raft_count), false});
            anchors.push_back({layer_z(slicing_params, config, source_layer_count - 1), false});
        }
        for (LayerIndex source_layer : mandatory_source_layers) {
            if (source_layer >= LayerIndex(raft_count) && source_layer < LayerIndex(source_layer_count))
                anchors.push_back({layer_z(slicing_params, config, size_t(source_layer)), false});
        }
        for (const SupportGeneratorLayer *contact : top_contacts) {
            if (contact == nullptr || contact->polygons.empty())
                continue;
            if (contact->print_z > EPSILON)
                anchors.push_back({contact->print_z, true});
        }
        // Keep model-contact clearance planes on the independent output grid.
        // Without these anchors, a configured bottom Z gap is rounded back to
        // the 0.20 mm source grid and the support root becomes too close.
        for (coordf_t z : bottom_clearance_planes)
            if (z > EPSILON && z < layer_z(slicing_params, config, source_layer_count - 1) - EPSILON)
                anchors.push_back({z, true});

        std::sort(anchors.begin(), anchors.end(), [](const Anchor &lhs, const Anchor &rhs) {
            return lhs.z < rhs.z;
        });
        std::vector<Anchor> merged_anchors;
        merged_anchors.reserve(anchors.size());
        for (const Anchor &anchor : anchors) {
            if (!std::isfinite(anchor.z) || anchor.z <= EPSILON) {
                failure_reason = "an output layer anchor has an invalid Z position";
                return std::nullopt;
            }
            if (!merged_anchors.empty() && std::abs(anchor.z - merged_anchors.back().z) <= EPSILON)
                merged_anchors.back().required = merged_anchors.back().required || anchor.required;
            else
                merged_anchors.push_back(anchor);
        }

        // Exact contact Z is mandatory. Remove nearby topology-grid anchors instead of
        // producing a sub-minimum contact layer (for example 0.03 mm for a 0.17 mm gap
        // on a 0.20 mm object grid).
        std::vector<Anchor> resolved_anchors;
        resolved_anchors.reserve(merged_anchors.size());
        for (const Anchor &anchor : merged_anchors) {
            bool keep_anchor = true;
            while (!resolved_anchors.empty() &&
                   anchor.z - resolved_anchors.back().z < min_height - EPSILON) {
                if (anchor.required && resolved_anchors.back().required) {
                    failure_reason = "required output layer anchors are closer than the minimum support layer height";
                    return std::nullopt;
                }
                if (anchor.required) {
                    resolved_anchors.pop_back();
                    continue;
                }
                keep_anchor = false;
                break;
            }
            if (keep_anchor)
                resolved_anchors.push_back(anchor);
        }

        std::vector<coordf_t> output_zs;
        output_zs.reserve(resolved_anchors.size());

        auto append_interval = [&](coordf_t target_z, bool enforce_minimum) -> bool {
            if (!std::isfinite(target_z) || target_z <= EPSILON)
                return false;
            if (output_zs.empty()) {
                output_zs.push_back(target_z);
                return true;
            }
            const coordf_t distance = target_z - output_zs.back();
            if (distance <= EPSILON)
                return std::abs(distance) <= EPSILON;
            const size_t steps = std::max<size_t>(1, size_t(std::ceil((distance - EPSILON) / max_height)));
            const coordf_t step = distance / coordf_t(steps);
            if (enforce_minimum && step < min_height - EPSILON)
                return false;
            const coordf_t start_z = output_zs.back();
            for (size_t step_idx = 1; step_idx <= steps; ++step_idx)
                output_zs.push_back(step_idx == steps ? target_z : start_z + coordf_t(step_idx) * step);
            return true;
        };

        for (const Anchor &anchor : resolved_anchors) {
            const bool first_non_raft_layer = output_zs.empty();
            if (!append_interval(anchor.z, !first_non_raft_layer)) {
                failure_reason = "an output layer interval cannot satisfy the configured support layer-height limits";
                return std::nullopt;
            }
        }

        if (output_zs.empty()) {
            failure_reason = "no output layers were planned";
            return std::nullopt;
        }

        result.m_entries.reserve(output_zs.size());
        for (size_t output_idx = 0; output_idx < output_zs.size(); ++output_idx) {
            const coordf_t print_z  = output_zs[output_idx];
            const coordf_t bottom_z = output_idx == 0 ? 0. : output_zs[output_idx - 1];
            const coordf_t height   = print_z - bottom_z;
            if (height <= EPSILON) {
                failure_reason = "the output plan contains a non-positive layer height";
                return std::nullopt;
            }

            LayerIndex source_begin = layer_idx_floor(slicing_params, config, bottom_z + EPSILON);
            LayerIndex source_end   = layer_idx_ceil(slicing_params, config, print_z - EPSILON);
            source_begin = std::clamp<LayerIndex>(source_begin, 0, LayerIndex(source_layer_count - 1));
            source_end   = std::clamp<LayerIndex>(source_end, source_begin, LayerIndex(source_layer_count - 1));
            result.m_entries.push_back({print_z, bottom_z, height, source_begin, source_end});
        }

        return result;
    }

    bool independent() const { return m_independent; }
    bool empty() const { return m_entries.empty(); }
    size_t size() const { return m_entries.size(); }
    const OrganicSupportLayerPlanEntry &operator[](size_t idx) const { return m_entries[idx]; }

    LayerIndex find_exact(coordf_t print_z) const
    {
        auto it = std::lower_bound(m_entries.begin(), m_entries.end(), print_z,
            [](const OrganicSupportLayerPlanEntry &entry, coordf_t z) { return entry.print_z < z - EPSILON; });
        return it != m_entries.end() && std::abs(it->print_z - print_z) <= EPSILON ?
            LayerIndex(std::distance(m_entries.begin(), it)) : -1;
    }

    LayerIndex at_or_below(coordf_t print_z) const
    {
        auto it = std::upper_bound(m_entries.begin(), m_entries.end(), print_z + EPSILON,
            [](coordf_t z, const OrganicSupportLayerPlanEntry &entry) { return z < entry.print_z; });
        return it == m_entries.begin() ? 0 : LayerIndex(std::distance(m_entries.begin(), it) - 1);
    }

    LayerIndex at_or_above_slice_z(coordf_t z) const
    {
        auto it = std::lower_bound(m_entries.begin(), m_entries.end(), z - EPSILON,
            [](const OrganicSupportLayerPlanEntry &entry, coordf_t value) {
                return 0.5 * (entry.bottom_z + entry.print_z) < value;
            });
        return it == m_entries.end() ? LayerIndex(m_entries.size()) : LayerIndex(std::distance(m_entries.begin(), it));
    }

    LayerIndex above_slice_z(coordf_t z) const
    {
        auto it = std::upper_bound(m_entries.begin(), m_entries.end(), z + EPSILON,
            [](coordf_t value, const OrganicSupportLayerPlanEntry &entry) {
                return value < 0.5 * (entry.bottom_z + entry.print_z);
            });
        return LayerIndex(std::distance(m_entries.begin(), it));
    }

    void initialize(SupportGeneratorLayer &layer, size_t layer_idx) const
    {
        const OrganicSupportLayerPlanEntry &entry = m_entries.at(layer_idx);
        layer.print_z  = entry.print_z;
        layer.bottom_z = entry.bottom_z;
        layer.height   = entry.height;
    }

private:
    std::vector<OrganicSupportLayerPlanEntry> m_entries;
    bool m_independent{false};
};

} // namespace TreeSupport3D
} // namespace Slic3r

#endif
