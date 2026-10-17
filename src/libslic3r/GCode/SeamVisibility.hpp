#ifndef libslic3r_SeamVisibility_hpp_
#define libslic3r_SeamVisibility_hpp_

#include "../TriangleMesh.hpp"
#include <utility>

namespace Slic3r::SeamPlacerImpl {

inline void transform_visibility_mesh(indexed_triangle_set &mesh, const Transform3d &transform)
{
    // Slicing quantizes the affine transform before applying it to float mesh
    // vertices. Use the same precision for visibility: a decimal round trip
    // must not bias a half-ULP translation through a double intermediate and
    // change the mesh used to choose aligned seams. Retain mirror correction.
    its_transform(mesh, transform.cast<float>());
    // Keep the original mirror classification and triangle ordering.
    if (transform.linear().determinant() < 0.)
        for (auto &triangle : mesh.indices)
            std::swap(triangle[0], triangle[1]);
}

} // namespace Slic3r::SeamPlacerImpl

#endif
