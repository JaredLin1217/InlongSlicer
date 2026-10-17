#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ClipperScanbeamTest {
using Path = std::vector<std::array<int64_t, 2>>;
using Paths = std::vector<Path>;
struct Result {
    bool succeeded;
    Paths closed;
    Paths open;
};
Result reference(const Paths &subjects, const Paths &clips, const Paths &open,
                 int operation, int fill_rule, bool preserve_collinear, bool reverse_solution);
}
