#include <catch2/catch_all.hpp>
#include "libslic3r/BridgeDetector.hpp"
#include <tbb/global_control.h>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace Slic3r;

TEST_CASE("Bridge scoring preserves exact geometry across worker limits", "[BridgeDetector][Regression]")
{
    const double rotation = GENERATE(0., 0.37);
    const double spacing = GENERATE(0.408, 0.612);
    const bool hollow = GENERATE(false, true);
    auto ellipse = [](double x, double y, size_t vertices) {
        Points points;
        for (size_t i = 0; i < vertices; ++i) {
            const double angle = 2. * PI * i / vertices;
            points.push_back(Point::new_scale(x * std::cos(angle), y * std::sin(angle)));
        }
        return Polygon(std::move(points));
    };
    ExPolygon region(ellipse(20., 8., 256));
    if (hollow) {
        Polygon hole = ellipse(8., 3., 128);
        hole.make_clockwise();
        region.holes.push_back(std::move(hole));
    }
    auto rectangle = [](double x0, double x1) {
        return ExPolygon(Polygon{Points{Point::new_scale(x0, -12.), Point::new_scale(x1, -12.),
            Point::new_scale(x1, 12.), Point::new_scale(x0, 12.)}});
    };
    ExPolygons regions{region}, anchors{rectangle(-22., -12.), rectangle(12., 22.)};
    for (ExPolygons *polygons : {&regions, &anchors})
        for (ExPolygon &polygon : *polygons) {
            polygon.rotate(rotation);
            polygon.translate(Point::new_scale(137., 143.));
        }
    struct Result {
        bool detected;
        uint64_t angle_bits;
        std::vector<Points> coverage, unsupported;
    };
    auto score = [&](size_t workers) {
        tbb::global_control control(tbb::global_control::max_allowed_parallelism, workers);
        BridgeDetector detector(regions, anchors, scale_(spacing));
        Result result{};
        result.detected = detector.detect_angle();
        static_assert(sizeof(result.angle_bits) == sizeof(detector.angle));
        std::memcpy(&result.angle_bits, &detector.angle, sizeof(result.angle_bits));
        for (const Polygon &polygon : detector.coverage()) result.coverage.push_back(polygon.points);
        for (const Polyline &line : detector.unsupported_edges()) result.unsupported.push_back(line.points);
        return result;
    };
    const Result serial = score(1), parallel = score(4);
    REQUIRE(serial.detected);
    CHECK(parallel.detected == serial.detected);
    CHECK(parallel.angle_bits == serial.angle_bits);
    CHECK(parallel.coverage == serial.coverage);
    CHECK(parallel.unsupported == serial.unsupported);
}
