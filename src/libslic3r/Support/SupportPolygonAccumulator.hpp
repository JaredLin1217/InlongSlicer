#ifndef slic3r_SupportPolygonAccumulator_hpp_
#define slic3r_SupportPolygonAccumulator_hpp_

#include "../Polygon.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Slic3r {

// Keep the original contour and vertex order while transferring their storage.
// Polygon's move constructor is not noexcept and its Points argument currently
// binds to MultiPoint's copying constructor. Repeated reserve()/append() would
// therefore copy every accumulated vertex, not just relocate polygon handles.
inline void append_support_polygons(Polygons &destination, Polygons &&source)
{
    if (destination.empty()) {
        destination = std::move(source);
        return;
    }
    if (source.size() > destination.max_size() - destination.size())
        throw std::length_error("Support polygon accumulation exceeds vector capacity");

    const size_t previous_size = destination.size();
    const size_t required_size = previous_size + source.size();
    if (required_size > destination.capacity()) {
        // Spare capacity contains only empty polygon handles, never extra point
        // arrays. Grow geometrically instead of reallocating for every branch.
        const size_t spare = destination.capacity() / 2;
        const size_t capacity = destination.capacity() > destination.max_size() - spare ?
            destination.max_size() : destination.capacity() + spare;
        Polygons grown;
        grown.reserve(std::max(required_size, capacity));
        grown.resize(required_size);
        for (size_t i = 0; i < previous_size; ++i)
            grown[i].points.swap(destination[i].points);
        destination.swap(grown);
    } else
        destination.resize(required_size);

    for (size_t i = 0; i < source.size(); ++i)
        destination[previous_size + i].points.swap(source[i].points);
    source.clear();
    source.shrink_to_fit();
}

} // namespace Slic3r

#endif
