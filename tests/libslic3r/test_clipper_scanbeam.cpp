#include <catch2/catch_all.hpp>
#include <clipper2/clipper.engine.h>

#include "clipper_scanbeam_reference.hpp"

#include <algorithm>
#include <random>

using namespace ClipperScanbeamTest;

TEST_CASE("Scanbeam acceleration preserves exact clipping output order and coordinates", "[ClipperScanbeam]")
{
    const int operation = GENERATE(1, 2, 3, 4);
    const int fill_rule = GENERATE(0, 1, 2, 3);
    const bool preserve_collinear = GENERATE(false, true);
    const bool reverse_solution = GENERATE(false, true);
    CAPTURE(operation, fill_rule, preserve_collinear, reverse_solution);
    std::mt19937 random(20261017);
    for (int scene = 0; scene < 32; ++scene) {
        CAPTURE(scene);
        Paths subjects, clips, open;
        for (int i = 0; i < 24; ++i) {
            const int64_t x = int64_t(random() % 200) - 100;
            const int64_t y = int64_t(random() % 12) * 10;
            const int64_t width = 1 + random() % 40;
            const int64_t height = 10 + random() % 30;
            // Shared scanlines, horizontal/collinear edges, ties, overlapping
            // contours, holes and self-intersections exercise both paths.
            Path path{{x, y}, {x + width / 2, y}, {x + width, y},
                      {x + width, y + height}, {x, y + height}};
            if ((scene + i) % 3 == 0)
                std::swap(path[2], path[3]);
            if ((scene + i) % 2 == 0)
                std::reverse(path.begin(), path.end());
            (i % 3 == 0 ? clips : subjects).push_back(std::move(path));
            if (i % 7 == 0)
                open.push_back({{x - 10, y - 10}, {x + width + 10, y + height + 10}});
        }
        const Result expected = reference(subjects, clips, open, operation, fill_rule,
                                          preserve_collinear, reverse_solution);
        using namespace Clipper2Lib;
        auto convert = [](const ClipperScanbeamTest::Paths &input) {
            Paths64 output;
            for (const ClipperScanbeamTest::Path &path : input) {
                Path64 &destination = output.emplace_back();
                for (const auto &point : path)
                    destination.emplace_back(point[0], point[1]);
            }
            return output;
        };
        Clipper64 clipper;
        clipper.PreserveCollinear(preserve_collinear);
        clipper.ReverseSolution(reverse_solution);
        clipper.AddSubject(convert(subjects));
        clipper.AddClip(convert(clips));
        clipper.AddOpenSubject(convert(open));
        Paths64 closed_result, open_result;
        REQUIRE(clipper.Execute(ClipType(operation), FillRule(fill_rule), closed_result, open_result) == expected.succeeded);
        auto compare = [](const Paths64 &actual, const ClipperScanbeamTest::Paths &expected_paths) {
            REQUIRE(actual.size() == expected_paths.size());
            for (size_t i = 0; i < actual.size(); ++i) {
                REQUIRE(actual[i].size() == expected_paths[i].size());
                for (size_t j = 0; j < actual[i].size(); ++j) {
                    REQUIRE(actual[i][j].x == expected_paths[i][j][0]);
                    REQUIRE(actual[i][j].y == expected_paths[i][j][1]);
                }
            }
        };
        compare(closed_result, expected.closed);
        compare(open_result, expected.open);
    }
}
