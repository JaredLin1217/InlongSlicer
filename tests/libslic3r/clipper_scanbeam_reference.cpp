#include "clipper_scanbeam_reference.hpp"

// Compile the untouched reference in a separate namespace. The production
// library uses the project CMake adapter; no vendor file is modified for tests.
#define Clipper2Lib InlongClipperReference
#include "../../deps_src/clipper2/Clipper2Lib/src/clipper.engine.cpp"
#undef Clipper2Lib

namespace ClipperScanbeamTest {
Result reference(const Paths &subjects, const Paths &clips, const Paths &open,
                 int operation, int fill_rule, bool preserve_collinear, bool reverse_solution)
{
    using namespace InlongClipperReference;
    auto convert = [](const Paths &input) {
        Paths64 result;
        for (const Path &path : input) {
            Path64 &destination = result.emplace_back();
            for (const auto &point : path)
                destination.emplace_back(point[0], point[1]);
        }
        return result;
    };
    Clipper64 clipper;
    clipper.PreserveCollinear(preserve_collinear);
    clipper.ReverseSolution(reverse_solution);
    clipper.AddSubject(convert(subjects));
    clipper.AddClip(convert(clips));
    clipper.AddOpenSubject(convert(open));
    Paths64 closed_result, open_result;
    Result result{clipper.Execute(ClipType(operation), FillRule(fill_rule), closed_result, open_result), {}, {}};
    auto copy_result = [](const Paths64 &input, Paths &output) {
        for (const Path64 &path : input) {
            Path &destination = output.emplace_back();
            for (const Point64 &point : path)
                destination.push_back({point.x, point.y});
        }
    };
    copy_result(closed_result, result.closed);
    copy_result(open_result, result.open);
    return result;
}
}
