#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../Tesselate.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"

#include "FillConcentric.hpp"
#include "FillCornerSmoothing.hpp"
#include <libslic3r/ShortestPath.hpp>

namespace Slic3r {

namespace {

Point interior_point(const ExPolygon &expolygon)
{
    const std::vector<Vec2d> triangles = triangulate_expolygon_2d(expolygon);
    double                   largest_area = 0.;
    Point                    result;

    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const double area = std::abs(cross2(triangles[i + 1] - triangles[i], triangles[i + 2] - triangles[i]));
        if (area > largest_area) {
            largest_area = area;
            const Vec2d centroid = (triangles[i] + triangles[i + 1] + triangles[i + 2]) / 3.;
            result = Point::new_scale(centroid);
        }
    }

    return result;
}

void append_concentric_center_paths(
    const ExPolygons &collapsed_regions,
    coord_t           line_width,
    size_t            first_path,
    Polylines        &polylines_out)
{
    if (line_width <= 0 || first_path > polylines_out.size() || collapsed_regions.empty())
        return;

    ExPolygons center_candidates;
    for (const ExPolygon &region : collapsed_regions)
        expolygons_append(center_candidates, offset_ex(region, -0.25f * float(line_width)));
    center_candidates = union_ex(center_candidates);
    if (center_candidates.empty())
        return;

    Polylines generated_paths(polylines_out.begin() + first_path, polylines_out.end());
    generated_paths = intersection_pl(
        generated_paths,
        offset_ex(center_candidates, 1.5f * float(line_width)));
    const ExPolygons center_regions = diff_ex(
        center_candidates,
        union_ex(offset(generated_paths, 0.5f * float(line_width))));

    for (const ExPolygon &region : center_regions) {
        const BoundingBox bbox = region.contour.bounding_box();
        if (!bbox.defined)
            continue;

        const Point size = bbox.size();
        const bool horizontal = size.x() >= size.y();
        const coord_t minor_span = horizontal ? size.y() : size.x();
        const coord_t major_span = horizontal ? size.x() : size.y();
        const size_t line_count = std::max<size_t>(1, size_t(std::ceil(double(minor_span) / line_width)));
        const double step = double(minor_span) / line_count;
        const coord_t margin = std::max(line_width, major_span);

        Polylines probes;
        probes.reserve(line_count);
        for (size_t line_idx = 0; line_idx < line_count; ++line_idx) {
            const coord_t minor = coord_t(std::llround(
                double(horizontal ? bbox.min.y() : bbox.min.x()) + (line_idx + 0.5) * step));
            probes.emplace_back(horizontal ?
                Polyline(Point(bbox.min.x() - margin, minor), Point(bbox.max.x() + margin, minor)) :
                Polyline(Point(minor, bbox.min.y() - margin), Point(minor, bbox.max.y() + margin)));
        }

        Polylines centerlines = intersection_pl(probes, region);
        centerlines.erase(
            std::remove_if(centerlines.begin(), centerlines.end(), [](const Polyline &polyline) {
                return !polyline.is_valid() || polyline.length() <= SCALED_EPSILON;
            }),
            centerlines.end());
        if (!centerlines.empty()) {
            append(polylines_out, std::move(centerlines));
            continue;
        }

        // A narrow diagonal region may miss the axis-aligned probes.
        Polylines medial_lines;
        region.medial_axis(
            std::max<double>(SCALED_EPSILON, 0.05 * line_width),
            2. * line_width,
            &medial_lines);
        medial_lines.erase(
            std::remove_if(medial_lines.begin(), medial_lines.end(), [](const Polyline &polyline) {
                return !polyline.is_valid() || polyline.length() <= SCALED_EPSILON;
            }),
            medial_lines.end());

        if (!medial_lines.empty()) {
            append(polylines_out, std::move(medial_lines));
            continue;
        }

        // Compact regions may collapse to a medial-axis point. Clip a short path
        // through a guaranteed interior point so the center still gets material.
        const Point center = interior_point(region);
        if (!region.contains(center))
            continue;

        const Polyline probe = size.x() >= size.y() ?
            Polyline(Point(bbox.min.x() - margin, center.y()), Point(bbox.max.x() + margin, center.y())) :
            Polyline(Point(center.x(), bbox.min.y() - margin), Point(center.x(), bbox.max.y() + margin));
        Polylines clipped = intersection_pl(probe, region);
        auto longest = std::max_element(clipped.begin(), clipped.end(), [](const Polyline &lhs, const Polyline &rhs) {
            return lhs.length() < rhs.length();
        });
        if (longest != clipped.end() && longest->is_valid() && longest->length() > SCALED_EPSILON)
            polylines_out.emplace_back(std::move(*longest));
    }
}

} // namespace

void FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t min_spacing = scale_(this->spacing) * params.multiline;
    coord_t distance = coord_t(min_spacing / params.density);
    
    if (params.density > 0.9999f && !params.dont_adjust) {
        distance = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * (params.multiline - 1) * this->spacing )));

    Polygons loops = to_polygons(contracted);

    ExPolygons collapsed_regions;
    ExPolygons last { contracted };
    while (! last.empty()) {
        ExPolygons next = offset2_ex(last, -(distance + min_spacing/2), +min_spacing/2);
        if (params.fill_concentric_gaps) {
            // Re-expanding the next inset reconstructs the parts of the current
            // contour that keep producing concentric loops. Any remainder has
            // collapsed at this step, including a narrow branch of a contour
            // whose wider part continues inward.
            if (next.empty())
                expolygons_append(collapsed_regions, last);
            else
                expolygons_append(collapsed_regions, diff_ex(last, offset_ex(next, float(distance))));
        }
        append(loops, to_polygons(next));
        last = std::move(next);
    }

    // Orca: round the corners of the loops. Unlike the other patterns these are never clipped to the
    // fill region - they are its offsets - so a corner may only be rounded where the curve replacing it
    // stays inside. Rounding cuts toward the inside of the turn, which around a hole, at a concave
    // feature or across a thin region is outside the fill and would put the extrusion over a wall.
    // The reach is capped at half the distance between two loops as well: a loop is as long as the
    // object, and a corner cut by half of its side would swallow the neighbouring loops.
    auto corner_stays_inside = [&contracted](const Vec2d &from, const Vec2d &to) {
        // The straight chord between the ends of the curve is the deepest the curve can cut.
        for (const double t : { 0.25, 0.5, 0.75 }) {
            const Vec2d  sample = from + t * (to - from);
            const Point  point(coord_t(sample.x()), coord_t(sample.y()));
            if (std::none_of(contracted.begin(), contracted.end(),
                             [&point](const ExPolygon &region) { return region.contains(point); }))
                return false;
        }
        return true;
    };
    smooth_polygons_corners(loops, params.smooth_factor, scaled<double>(params.resolution), 0.5 * distance,
                            corner_stays_inside);

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained_outside_in(loops);

    // Orca: an outward fill order prints the innermost loops first instead.
    if (params.fill_order == SurfaceFillOrder::Outward)
        std::reverse(loops.begin(), loops.end());
    
    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();
    Point last_pos(0, 0);
    for (const Polygon &loop : loops) {
        polylines_out.emplace_back(loop.split_at_index(last_pos.nearest_point_index(loop.points)));
        last_pos = polylines_out.back().last_point();
    }

    // Apply multiline offset if needed
    multiline_fill(polylines_out, params, spacing);

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());

    if (params.fill_concentric_gaps) {
        const coord_t line_width = params.flow.scaled_width() > 0 ? params.flow.scaled_width() : min_spacing;
        append_concentric_center_paths(collapsed_regions, line_width, iPathFirst, polylines_out);
    }
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

void FillConcentric::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // no rotation is supported for this infill pattern
    Point   bbox_size = expolygon.contour.bounding_box().size();
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    if (params.density > 0.9999f && !params.dont_adjust) {
        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = offset(expolygon, float(min_spacing) / 2.f);

        double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 1.0 * min_nozzle_diameter;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;

        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops) {
            if (loop.empty())
                continue;
            for (const Arachne::ExtrusionLine& wall : loop)
                all_extrusions.emplace_back(&wall);
        }

        // Orca: a forced fill order prints the loops in strictly monotonic depth order so
        // that surfaces broken up by holes or slots cannot hop outward and back inward.
        const bool forced_fill_order = params.fill_order != SurfaceFillOrder::Default;
        if (forced_fill_order) {
            const bool outward = params.fill_order == SurfaceFillOrder::Outward;
            std::stable_sort(all_extrusions.begin(), all_extrusions.end(),
                             [outward](const Arachne::ExtrusionLine *a, const Arachne::ExtrusionLine *b) {
                                 return outward ? a->inset_idx > b->inset_idx : a->inset_idx < b->inset_idx;
                             });
        }

        // Split paths using a nearest neighbor search.
        size_t firts_poly_idx = thick_polylines_out.size();
        Point  last_pos(0, 0);
        for (const Arachne::ExtrusionLine* extrusion : all_extrusions) {
            if (extrusion->empty())
                continue;

            ThickPolyline thick_polyline = Arachne::to_thick_polyline(*extrusion);
            if (extrusion->is_closed)
                thick_polyline.start_at_index(last_pos.nearest_point_index(thick_polyline.points));
            thick_polylines_out.emplace_back(std::move(thick_polyline));
            last_pos = thick_polylines_out.back().last_point();
        }

        // clip the paths to prevent the extruder from getting exactly on the first point of the loop
        // Keep valid paths only.
        size_t j = firts_poly_idx;
        for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
            thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());

        if (!forced_fill_order)
            reorder_by_shortest_traverse(thick_polylines_out);
    }
    else {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
    }
}

} // namespace Slic3r
