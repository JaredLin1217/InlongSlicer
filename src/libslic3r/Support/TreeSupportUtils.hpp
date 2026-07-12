#ifndef slic3r_TreeSupportUtils_hpp_
#define slic3r_TreeSupportUtils_hpp_

#include "../ExPolygon.hpp"
#include "../Flow.hpp"

namespace Slic3r::TreeSupportInternal {

// Returns true only for non-printable Roof1stLayer dust created outside the
// original manual-enforcer contact region.
bool should_discard_manual_roof_fragment(
    bool from_manual_contact,
    const ExPolygon &fragment,
    const ExPolygons &original_contact_regions,
    const Flow &flow);

} // namespace Slic3r::TreeSupportInternal

#endif // slic3r_TreeSupportUtils_hpp_
