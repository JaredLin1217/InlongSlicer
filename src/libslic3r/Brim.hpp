#ifndef slic3r_Brim_hpp_
#define slic3r_Brim_hpp_

#include "ExPolygon.hpp"
#include "ObjectID.hpp"
#include "Point.hpp"
#include "Polygon.hpp"

#include<map>
#include <utility>
#include<vector>

namespace Slic3r {

class Print;
class ExtrusionEntityCollection;
class PrintTryCancel;

// Produce brim lines around those objects, that have the brim enabled.
// Collect islands_area to be merged into the final 1st layer convex hull.
void make_brim(const Print& print, PrintTryCancel try_cancel,
    Polygons& islands_area, std::map<ObjectID, ExtrusionEntityCollection>& brimMap,
    std::map<ObjectInstanceID, ExtrusionEntityCollection>& brimMapByInstance,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec,
    std::vector<unsigned int>& printExtruders,
    std::map<ObjectInstanceID, ExPolygons>* objectBrimAreasByInstanceOut = nullptr);

ExtrusionEntityCollection makeBrimInfill(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);
ExtrusionEntityCollection makeBrimInfillFromPlateCoordinates(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area);

// Derive bed support paths after the final, possibly combined brim is planned.
// Source support paths remain unchanged so editing or disabling brim is safe.
ExtrusionEntityCollection support_outside_brim(const ExtrusionEntityCollection &support, const Polygons &brim_coverage,
    const ExPolygons &support_regions = {});
void connect_bed_support_paths(ExtrusionEntityCollection &result,
    const ExtrusionEntityCollection &source, const Polygons &brim_coverage,
    const ExPolygons &support_regions = {});
bool normalize_bed_support_widths(ExtrusionEntityCollection &result,
    const ExtrusionEntityCollection &source, const Polygons &brim_coverage,
    const ExPolygons &support_regions);
void make_brim_support_overrides(const Print &print,
    const std::vector<std::pair<ObjectID, unsigned int>> &objects,
    const std::vector<unsigned int> &extruders,
    std::map<ObjectInstanceID, ExtrusionEntityCollection> &output);

// BBS: automatically make brim
ExtrusionEntityCollection make_brim_auto(const Print &print, PrintTryCancel try_cancel, Polygons &islands_area);

} // Slic3r

#endif // slic3r_Brim_hpp_
