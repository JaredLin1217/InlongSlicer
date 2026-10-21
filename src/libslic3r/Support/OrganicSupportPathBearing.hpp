#ifndef slic3r_OrganicSupportPathBearing_hpp_
#define slic3r_OrganicSupportPathBearing_hpp_

#include "../ClipperUtils.hpp"
#include "../BoundingBox.hpp"
#include "../Line.hpp"
#include "../Polyline.hpp"

#include <algorithm>

namespace Slic3r::TreeSupport3D {

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
    const double permitted_gap = complete_contour ? std::max(2. * coordinate_tolerance, line_width * 0.05) : line_width;
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
            return true;
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
