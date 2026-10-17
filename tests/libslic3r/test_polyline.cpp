#include <catch2/catch_all.hpp>

#include "libslic3r/Polyline.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;

TEST_CASE("Simplification preserves small closed extrusion contours", "[Polyline][Regression]")
{
    const double scale = GENERATE(0.25, 1., 4.);
    const double tolerance = GENERATE(0.001, 0.01, 0.05);
    const bool arcs = GENERATE(false, true);
    CAPTURE(scale, tolerance, arcs);

    // A narrow closed tip, also exercised after translation and scaling. A
    // tolerance larger than the contour must not turn it into a stationary move.
    Polyline3 contour(Points3{
        Point3::new_scale(40., 120., 0.),
        Point3::new_scale(40. + 0.009 * scale, 120. - 0.002 * scale, 0.),
        Point3::new_scale(40. + 0.004 * scale, 120. - 0.002 * scale, 0.),
        Point3::new_scale(40., 120., 0.)});
    const auto first = contour.first_point();
    if (arcs)
        contour.simplify_by_fitting_arc(scaled<double>(tolerance));
    else
        contour.simplify(scaled<double>(tolerance));

    REQUIRE(contour.first_point() == first);
    REQUIRE(contour.last_point() == first);
    REQUIRE(contour.length() > 0.);
    if (std::none_of(contour.fitting_result.begin(), contour.fitting_result.end(),
                     [](const PathFittingData &fit) {
                         return fit.path_type == EMovePathType::Arc_move_cw || fit.path_type == EMovePathType::Arc_move_ccw;
                     }))
        REQUIRE(contour.points.size() >= 4);
    for (const auto &fit : contour.fitting_result) {
        REQUIRE(fit.start_point_index < fit.end_point_index);
        REQUIRE(fit.end_point_index < contour.points.size());
    }
}

TEST_CASE("Open paths still simplify and circular paths still fit arcs", "[Polyline]")
{
    const bool arcs = GENERATE(false, true);
    Polyline3 line(Points3{Point3::new_scale(0., 0., 0.), Point3::new_scale(5., 0.001, 0.),
                          Point3::new_scale(10., 0., 0.)});
    if (arcs)
        line.simplify_by_fitting_arc(scaled<double>(0.01));
    else
        line.simplify(scaled<double>(0.01));
    REQUIRE(line.points.size() == 2);
    REQUIRE_THAT(unscale<double>(line.length()), Catch::Matchers::WithinAbs(10., 1e-5));

    // Keep the source chord error below the fitting tolerance as well.
    Polyline3 circle;
    for (size_t i = 0; i <= 32; ++i) {
        const double angle = 0.5 * PI * double(i) / 32.;
        circle.points.push_back(Point3::new_scale(10. * std::cos(angle), 10. * std::sin(angle), 0.));
    }
    circle.simplify_by_fitting_arc(scaled<double>(0.01));
    REQUIRE(std::any_of(circle.fitting_result.begin(), circle.fitting_result.end(),
                        [](const PathFittingData &fit) {
                            return fit.path_type == EMovePathType::Arc_move_cw || fit.path_type == EMovePathType::Arc_move_ccw;
                        }));
}
