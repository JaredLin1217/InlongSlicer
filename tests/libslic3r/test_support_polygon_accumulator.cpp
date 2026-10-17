#include <catch2/catch_all.hpp>

#include "libslic3r/Support/SupportPolygonAccumulator.hpp"

#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("Support polygon accumulation preserves contours and transfers point storage",
          "[SupportPolygonAccumulator]")
{
    const size_t initial_capacity = GENERATE(size_t(0), size_t(7), size_t(128));
    const size_t batch_size = GENERATE(size_t(1), size_t(7), size_t(64));
    CAPTURE(initial_capacity, batch_size);

    Polygons actual, expected;
    actual.reserve(initial_capacity);
    std::vector<const Point *> buffers;
    if (initial_capacity != 0) {
        actual.emplace_back(Points{{-20, 0}, {-10, 0}, {-10, 10}, {-20, 10}});
        expected.push_back(actual.back());
        buffers.push_back(actual.back().points.data());
    }
    for (size_t batch = 0; batch < 8; ++batch) {
        Polygons incoming;
        // Capture stable source buffers after allocating the complete batch.
        incoming.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i) {
            const coord_t x = coord_t((batch * batch_size + i) * 20);
            incoming.emplace_back(Points{{x, 0}, {x + 10, 0}, {x + 10, 10}, {x, 10}});
            if (i % 2 == 0)
                incoming.back().reverse();
            buffers.push_back(incoming.back().points.data());
            expected.push_back(incoming.back());
        }

        append_support_polygons(actual, std::move(incoming));
        REQUIRE(incoming.empty());
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            REQUIRE(actual[i].points == expected[i].points);
            // Repeated growth must retain each transferred point buffer rather
            // than allocating and copying every previous contour again.
            REQUIRE(actual[i].points.data() == buffers[i]);
        }
    }

    Polygons empty;
    append_support_polygons(actual, std::move(empty));
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i)
        REQUIRE(actual[i].points == expected[i].points);
}
