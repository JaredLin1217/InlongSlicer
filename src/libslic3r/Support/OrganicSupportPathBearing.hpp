#ifndef slic3r_OrganicSupportPathBearing_hpp_
#define slic3r_OrganicSupportPathBearing_hpp_

#include "../ClipperUtils.hpp"
#include "../BoundingBox.hpp"
#include "../Line.hpp"
#include "../Polyline.hpp"
#include "../AABBTreeLines.hpp"

#include <algorithm>

namespace Slic3r::TreeSupport3D {

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

// Return only spans that cannot be printed from the existing foundation.
// Straight bridges need support at both ends; a curved cantilever does not
// acquire a foundation merely because another part of its loop is supported.
inline Polylines organic_support_unbacked_spans(const Polyline &line, const Polygons &foundation,
    double lateral_reach, double line_width, double coordinate_tolerance, double relative_simplification = 0.,
    double maximum_bridge_deviation = -1., bool complete_contour = false)
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
    const Polygons endpoints = offset(reachable, coordinate_tolerance);
    spans.erase(std::remove_if(spans.begin(), spans.end(), [&](const Polyline &span) {
        if (span.length() <= permitted_gap)
            return !complete_contour ||
                (contains(endpoints, span.first_point()) && contains(endpoints, span.last_point()));
        if (complete_contour)
            return false;
        if (!contains(endpoints, span.first_point()) || !contains(endpoints, span.last_point()))
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

} // namespace Slic3r::TreeSupport3D

#endif
