#ifndef slic3r_OrganicSupportPathBearing_hpp_
#define slic3r_OrganicSupportPathBearing_hpp_

#include "../ClipperUtils.hpp"
#include "../BoundingBox.hpp"
#include "../Line.hpp"
#include "../Polyline.hpp"
#include "../ExtrusionEntity.hpp"
#include "../AABBTreeLines.hpp"
#include "../AABBTreeIndirect.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>

namespace Slic3r::TreeSupport3D {

struct OrganicSupportPolygonGeometry {
    using PolygonTree = AABBTreeIndirect::Tree<2, coord_t>;

    Polygons polygons;
    std::vector<BoundingBox> polygon_bounds;
    BoundingBox bounds;
    size_t vertex_count{0};
    PolygonTree polygon_tree;

    OrganicSupportPolygonGeometry() = default;
    explicit OrganicSupportPolygonGeometry(const Polygons &input) : polygons(input) { initialize(); }
    explicit OrganicSupportPolygonGeometry(Polygons &&input) : polygons(std::move(input)) { initialize(); }

    size_t spatial_bytes() const
    {
        return polygon_bounds.capacity() * sizeof(BoundingBox) +
            polygon_tree.nodes().capacity() * sizeof(PolygonTree::Node);
    }

private:
    void initialize()
    {
        polygon_bounds.reserve(polygons.size());
        std::vector<AABBTreeIndirect::BoundingBoxWrapper> boxes;
        boxes.reserve(polygons.size());
        for (size_t index = 0; index < polygons.size(); ++index) {
            const Polygon &polygon = polygons[index];
            vertex_count += polygon.size();
            const BoundingBox polygon_box = get_extents(polygon);
            polygon_bounds.push_back(polygon_box);
            if (polygon_box.defined) {
                bounds.merge(polygon_box);
                boxes.emplace_back(index, polygon_box);
            }
        }
        polygon_tree.build_modify_input(boxes);
    }
};

class OrganicSupportOrderedPlane {
public:
    void append(const Polygons &polygons)
    {
        if (polygons.empty())
            return;
        const size_t index = m_entries.size();
        m_entries.emplace_back(polygons);
        const BoundingBox &bounds = m_entries.back().bounds;
        if (bounds.defined)
            m_min_x.emplace(bounds.min.x(), index);
    }

    size_t size() const { return m_entries.size(); }

    template<typename Callback>
    void for_each_geometry(const BoundingBox &query, Callback &&callback) const
    {
        if (!query.defined)
            return;
        std::vector<size_t> candidates;
        const auto end = m_min_x.upper_bound(query.max.x());
        for (auto it = m_min_x.begin(); it != end; ++it) {
            const size_t index = it->second;
            if (m_entries[index].bounds.overlap(query))
                candidates.push_back(index);
        }
        std::sort(candidates.begin(), candidates.end());
        for (const size_t index : candidates)
            callback(m_entries[index]);
    }

private:
    std::vector<OrganicSupportPolygonGeometry> m_entries;
    std::multimap<coord_t, size_t> m_min_x;
};

struct OrganicSupportClippingStats {
    size_t candidate_polygons{0};
    size_t candidate_vertices{0};
};

inline Polygons organic_support_clipped_candidates(const BoundingBox &bead_bounds,
    const OrganicSupportPolygonGeometry &material, OrganicSupportClippingStats *stats = nullptr)
{
    if (!bead_bounds.defined || !material.bounds.defined || !material.bounds.overlap(bead_bounds))
        return {};
    const OrganicSupportPolygonGeometry::PolygonTree::BoundingBox query(bead_bounds.min, bead_bounds.max);
    std::vector<size_t> overlapping;
    AABBTreeIndirect::traverse(material.polygon_tree,
        [&query](const OrganicSupportPolygonGeometry::PolygonTree::Node &node) { return node.bbox.intersects(query); },
        [&overlapping](const OrganicSupportPolygonGeometry::PolygonTree::Node &node) {
            overlapping.push_back(node.idx);
            return true;
        });
    std::sort(overlapping.begin(), overlapping.end());
    Polygons candidates;
    candidates.reserve(overlapping.size());
    for (const size_t index : overlapping) {
        if (!material.polygon_bounds[index].overlap(bead_bounds))
            continue;
        if (stats != nullptr) {
            ++stats->candidate_polygons;
            stats->candidate_vertices += material.polygons[index].size();
        }
        Polygon candidate = ClipperUtils::clip_clipper_polygon_with_subject_bbox(material.polygons[index], bead_bounds);
        if (!candidate.empty())
            candidates.push_back(std::move(candidate));
    }
    return candidates;
}

inline Polygons organic_support_clipped_intersection(const Polygons &bead, const BoundingBox &bead_bounds,
    const OrganicSupportPolygonGeometry &material, OrganicSupportClippingStats *stats = nullptr)
{
    if (bead.empty() || material.polygons.empty())
        return {};
    const Polygons candidates = organic_support_clipped_candidates(bead_bounds.inflated(SCALED_EPSILON), material, stats);
    return candidates.empty() ? Polygons{} : intersection(bead, candidates);
}

inline Polygons organic_support_clipped_intersection(const Polygons &bead, const OrganicSupportPolygonGeometry &material,
    OrganicSupportClippingStats *stats = nullptr)
{
    return organic_support_clipped_intersection(bead, get_extents(bead), material, stats);
}

struct OrganicSupportBeadGeometry {
    Polygons polygons;
    BoundingBox bounds;
    size_t vertex_count{0};
};

inline double organic_support_bead_arc_tolerance(double config_resolution)
{
    return std::min(scaled<double>(0.03), config_resolution);
}

inline std::shared_ptr<const OrganicSupportBeadGeometry> make_organic_support_bead_geometry(const Polyline &line, float width,
    double arc_tolerance = scale_(0.01))
{
    auto result = std::make_shared<OrganicSupportBeadGeometry>();
    result->polygons = offset(line, float(scale_(width * 0.5)), jtRound, arc_tolerance, etOpenRound);
    result->bounds = get_extents(result->polygons);
    for (const Polygon &polygon : result->polygons)
        result->vertex_count += polygon.size();
    return result;
}

inline Polygons organic_support_accumulate_bead_polygons(
    const std::vector<std::shared_ptr<const OrganicSupportBeadGeometry>> &beads)
{
    Polygons result;
    for (const auto &bead : beads)
        append(result, bead->polygons);
    return result;
}

struct OrganicSupportRoadGeometry {
    Polyline line;
    BoundingBox bead_bounds;
    std::vector<std::shared_ptr<const OrganicSupportBeadGeometry>> path_beads;
    double width{std::numeric_limits<double>::max()};
    double max_width{0.};
    double latest_bottom{-std::numeric_limits<double>::max()};
    bool continuous{true};
    bool uniform{true};
};

inline const ExtrusionPath &organic_support_road_path(const ExtrusionPath &path) { return path; }
inline const ExtrusionPath &organic_support_road_path(std::reference_wrapper<const ExtrusionPath> path) { return path.get(); }

template<typename PathRange, typename BeadProvider>
inline OrganicSupportRoadGeometry make_organic_support_road_geometry(const PathRange &paths, double target_print_z,
    BeadProvider &&bead_provider)
{
    OrganicSupportRoadGeometry result;
    result.path_beads.reserve(paths.size());
    for (const auto &entry : paths) {
        const ExtrusionPath &path = organic_support_road_path(entry);
        Polyline part = path.polyline.to_polyline();
        auto bead = bead_provider(path, part);
        result.bead_bounds.merge(bead->bounds);
        if (result.line.empty())
            result.line = std::move(part);
        else if (result.line.last_point() == part.first_point())
            result.line.points.insert(result.line.points.end(), part.points.begin() + 1, part.points.end());
        else
            result.continuous = false;
        result.uniform &= std::abs(path.width - organic_support_road_path(paths.front()).width) < EPSILON &&
            std::abs(path.height - organic_support_road_path(paths.front()).height) < EPSILON;
        result.width = std::min(result.width, double(path.width));
        result.max_width = std::max(result.max_width, double(path.width));
        result.latest_bottom = std::max(result.latest_bottom, target_print_z - path.height);
        result.path_beads.emplace_back(std::move(bead));
    }
    return result;
}

inline OrganicSupportRoadGeometry make_organic_support_road_geometry(const ExtrusionPaths &paths, double target_print_z)
{
    return make_organic_support_road_geometry(paths, target_print_z, [](const ExtrusionPath &path, const Polyline &line) {
        return make_organic_support_bead_geometry(line, path.width);
    });
}

// An open straight bridge with both ends strictly inside the real foundation
// passes the native span checker: interior clipped spans have two anchors and
// zero bridge deviation. Stay clear of polygon rounding and leave polylines,
// boundary points and cantilevers to the full checker.
inline bool organic_support_definitely_bridged(const Polyline &line,
    const AABBTreeLines::LinesDistancer<Linef> &foundation, double coordinate_tolerance)
{
    if (line.size() != 2 || !line.is_valid() || foundation.get_lines().empty()) return false;
    const double guard = std::max(double(4 * SCALED_EPSILON), 2. * coordinate_tolerance);
    auto anchored = [&](const Point &point) {
        const double distance = foundation.distance_from_lines<true>(point.cast<double>());
        return std::isfinite(distance) && distance < -guard;
    };
    return anchored(line.first_point()) && anchored(line.last_point());
}

// Positive certificate only: a tube around every source segment lies strictly
// inside real earlier material. Distance to a closed boundary is 1-Lipschitz,
// so a midpoint ball certifies a whole interval without polygon offsets. Any
// ambiguous sign, narrow clearance or exhausted query budget uses the original
// geometric checker instead. Disjoint inscribed disks bound actual bearing area.
inline bool organic_support_definitely_backed(const Polyline &line,
    const AABBTreeLines::LinesDistancer<Linef> &foundation, double line_width, double minimum_area)
{
    if (!line.is_valid() || foundation.get_lines().empty()) return false;
    const double margin = line_width * 0.125;
    const double guard = std::max(double(4 * SCALED_EPSILON), double(scale_(0.004)));
    const double radius = margin - guard;
    if (radius <= guard) return false;
    size_t remaining = std::min<size_t>(32768, std::max<size_t>(1024, 4 * line.size()));
    auto covered = [&](auto &&self, const Vec2d &a, const Vec2d &b, unsigned depth) -> bool {
        if (remaining == 0) return false;
        --remaining;
        const Vec2d middle = 0.5 * (a + b);
        const double clearance = -foundation.distance_from_lines<true>(middle);
        if (!std::isfinite(clearance) || clearance <= margin + guard) return false;
        if (clearance > 0.5 * (b - a).norm() + margin + guard) return true;
        return depth < 10 && self(self, a, middle, depth + 1) && self(self, middle, b, depth + 1);
    };
    double longest = 0.;
    for (size_t i = 1; i < line.size(); ++i) {
        const Vec2d a = line.points[i - 1].cast<double>(), b = line.points[i].cast<double>();
        if (!covered(covered, a, b, 0)) return false;
        longest = std::max(longest, (b - a).norm());
    }
    const double required = minimum_area + sqr(guard);
    // A segment capsule contains its rectangle and two half diamonds.
    if (2. * radius * longest + 2. * sqr(radius) > required) return true;
    std::vector<Vec2d> disks;
    for (const Point &point : line.points) {
        const Vec2d center = point.cast<double>();
        if (std::all_of(disks.begin(), disks.end(), [&](const Vec2d &other) {
                return (center - other).squaredNorm() > 4. * sqr(radius);
            })) {
            disks.push_back(center);
            if (2. * disks.size() * sqr(radius) > required) return true;
            if (disks.size() >= 64) return false;
        }
    }
    return false;
}

// Clip a filled cross-section, then extract complete contour roads. Clipping
// its boundary as an open polyline leaves hooks instead of printable trunks.
// An open continuation becomes a narrow closed strut, not a dangling road.
inline Polylines organic_support_closed_contours(const Polyline &source, const Polygons &allowed, double width)
{
    if (!source.is_valid() || allowed.empty())
        return {};
    Polygons region;
    if (source.is_closed()) {
        Polygon polygon(Points(source.points.begin(), source.points.end() - 1));
        polygon.make_counter_clockwise();
        region = union_(Polygons{std::move(polygon)});
    } else
        // Keep the source inside the printed strut, rather than exactly on its
        // inner bead edge where later path fitting can remove all bearing.
        region = offset(source, float(width * 0.25), jtRound, scale_(0.01));
    Polylines result;
    for (const ExPolygon &section : intersection_ex(region, allowed)) {
        if (section.area() < sqr(width) * 0.002)
            continue;
        result.push_back(section.contour.split_at_first_point());
        for (const Polygon &hole : section.holes)
            result.push_back(hole.split_at_first_point());
    }
    return result;
}

struct OrganicSupportSpanStats {
    size_t endpoint_offset_calls{0};
};

// Return only spans that cannot be printed from the existing foundation.
// Straight bridges need support at both ends; a curved cantilever does not
// acquire a foundation merely because another part of its loop is supported.
inline Polylines organic_support_unbacked_spans(const Polyline &line, const Polygons &foundation,
    double lateral_reach, double line_width, double coordinate_tolerance, double relative_simplification = 0.,
    double maximum_bridge_deviation = -1., bool complete_contour = false, OrganicSupportSpanStats *stats = nullptr)
{
    if (!line.is_valid())
        return {};
    // Even a closed wall can cross a sub-bead span between two anchors. Such
    // a span is shorter than one deposited bead width, not a new cantilever.
    // Keep the stricter closed-wall rule for longer unsupported sections.
    const double permitted_gap = line_width;
    if (foundation.empty())
        return line.length() <= permitted_gap ? Polylines{} : Polylines{line};
    BoundingBox window = get_extents(line);
    window.offset(lateral_reach + coordinate_tolerance);
    const Polygons reachable = offset(intersection(foundation, Polygons{window.polygon()}), lateral_reach);
    Polylines spans = diff_pl(line, reachable);
    if (spans.empty())
        return {};
    // Clipping a closed road at its seam must not turn one bridge into two
    // apparently unsupported, open-ended cantilevers.
    if (line.first_point() == line.last_point() && spans.size() > 1) {
        const Point &seam = line.first_point();
        for (size_t i = 0; i < spans.size(); ++i) {
            if (spans[i].first_point() != seam && spans[i].last_point() != seam)
                continue;
            if (spans[i].first_point() == seam)
                spans[i].reverse();
            for (size_t j = i + 1; j < spans.size(); ++j) {
                if (spans[j].first_point() != seam && spans[j].last_point() != seam)
                    continue;
                if (spans[j].last_point() == seam)
                    spans[j].reverse();
                spans[i].points.insert(spans[i].points.end(), spans[j].points.begin() + 1, spans[j].points.end());
                spans.erase(spans.begin() + j);
                break;
            }
            break;
        }
    }
    Polygons endpoints;
    bool endpoints_ready = false;
    auto endpoint_region = [&]() -> const Polygons & {
        if (!endpoints_ready) {
            endpoints = offset(reachable, coordinate_tolerance);
            endpoints_ready = true;
            if (stats != nullptr)
                ++stats->endpoint_offset_calls;
        }
        return endpoints;
    };
    spans.erase(std::remove_if(spans.begin(), spans.end(), [&](const Polyline &span) {
        if (span.length() <= permitted_gap)
            return !complete_contour ||
                (contains(endpoint_region(), span.first_point()) && contains(endpoint_region(), span.last_point()));
        if (complete_contour)
            return false;
        if (!contains(endpoint_region(), span.first_point()) || !contains(endpoint_region(), span.last_point()))
            return false;
        const Line bridge(span.first_point(), span.last_point());
        const double requested_deviation = maximum_bridge_deviation < 0. ? line_width * 0.25 : maximum_bridge_deviation;
        const double deviation = std::min(requested_deviation, std::max(0., line_width * 0.5 - relative_simplification));
        return std::all_of(span.points.begin(), span.points.end(), [&](const Point &point) {
            return line_alg::distance_to_squared(bridge, point) <= sqr(deviation + coordinate_tolerance);
        });
    }), spans.end());
    return spans;
}

inline std::vector<bool> organic_support_select_brace_candidates(const std::vector<std::vector<size_t>> &coverage,
    const std::vector<double> &lengths)
{
    std::vector<std::vector<size_t>> sample_candidates;
    std::vector<size_t> gains(coverage.size(), 0);
    for (size_t candidate = 0; candidate < coverage.size(); ++candidate) {
        gains[candidate] = coverage[candidate].size();
        for (const size_t sample : coverage[candidate]) {
            if (sample >= sample_candidates.size())
                sample_candidates.resize(sample + 1);
            sample_candidates[sample].push_back(candidate);
        }
    }
    std::vector<bool> covered(sample_candidates.size(), false), selected(coverage.size(), false);
    for (;;) {
        size_t best = coverage.size(), best_gain = 0;
        double best_length = std::numeric_limits<double>::max();
        for (size_t candidate = 0; candidate < coverage.size(); ++candidate) {
            if (selected[candidate])
                continue;
            const size_t gain = gains[candidate];
            if (gain > best_gain || (gain > 0 && gain == best_gain && lengths[candidate] < best_length)) {
                best = candidate;
                best_gain = gain;
                best_length = lengths[candidate];
            }
        }
        if (best == coverage.size())
            break;
        selected[best] = true;
        for (const size_t sample : coverage[best]) {
            if (covered[sample])
                continue;
            covered[sample] = true;
            for (const size_t candidate : sample_candidates[sample])
                if (gains[candidate] > 0)
                    --gains[candidate];
        }
    }
    return selected;
}

} // namespace Slic3r::TreeSupport3D

#endif
