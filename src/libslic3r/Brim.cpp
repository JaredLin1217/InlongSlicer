#include "BoundingBox.hpp"
#include "BrimEarsPoint.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "ExPolygon.hpp"
#include "Flow.hpp"
#include "Support/SupportCommon.hpp"
#include "VariableWidth.hpp"
#include "Geometry.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Geometry/MedialAxis.hpp"
#include "ExtrusionEntity.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "ObjectID.hpp"
#include "Polygon.hpp"
#include "Line.hpp"
#include "MultiPoint.hpp"
#include "Polyline.hpp"
#include "Print.hpp"
#include "PrintBase.hpp"
#include "ShortestPath.hpp"
#include "libslic3r.h"
#include "PrintConfig.hpp"
#include "MaterialType.hpp"
#include "Model.hpp"
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <tbb/parallel_for.h>

#include <boost/log/trivial.hpp>
#include <utility>
#include <vector>
#include "Config.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "SurfaceCollection.hpp"
#include "TriangleMesh.hpp"

#ifndef NDEBUG
    // #define BRIM_DEBUG_TO_SVG
#endif

#if defined(BRIM_DEBUG_TO_SVG)
    #include "SVG.hpp"
#endif

namespace Slic3r {

static void append_and_translate(ExPolygons &dst, const ExPolygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    expolygons_append(dst, src);

    Point instance_shift = instance.shift_without_plate_offset();
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance_shift);
}
// INLONG: Translate the brim area into print coordinates and store it per instance.
static void append_and_translate(const ExPolygons& src, const PrintInstance& instance,
    size_t instance_idx, std::map<ObjectInstanceID, ExPolygons>& brimAreaMap) {
    ExPolygons srcShifted = src;
    Point instance_shift = instance.shift_without_plate_offset();
    for (ExPolygon& expoly : srcShifted)
        expoly.translate(instance_shift);
    expolygons_append(brimAreaMap[{ instance.print_object->id(), instance_idx }], std::move(srcShifted));
}

static void append_and_translate(Polygons &dst, const Polygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    polygons_append(dst, src);
    Point instance_shift = instance.shift_without_plate_offset();
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance_shift);
}

//ORCA: Brim can follow the post-EFC outline when enabled.
static bool use_brim_efc_outline(const PrintObject &object)
{
    return object.config().brim_use_efc_outline.value
        && object.config().elefant_foot_compensation.value > 0.
        && object.config().elefant_foot_compensation_layers.value > 0
        && object.config().raft_layers.value == 0;
}

//ORCA: Helper for projecting painted ears to the EFC outline.
static bool closest_point_on_expolygons(const ExPolygons &polygons, const Point &from, Point &closest_out)
{
    double min_dist2 = std::numeric_limits<double>::max();
    bool found = false;

    for (const ExPolygon &poly : polygons) {
        for (int i = 0; i < poly.num_contours(); ++i) {
            const Lines lines = poly.contour_or_hole(i).lines();
            for (const Line &line : lines) {
                Point candidate;
                const double dist2 = line.distance_to_squared(from, &candidate);
                if (dist2 < min_dist2) {
                    min_dist2 = dist2;
                    closest_out = candidate;
                    found = true;
                }
            }
        }
    }
    return found;
}

//ORCA: Helper for matching painted ears to their original island before EFC projection.
static int find_containing_expolygon_index(const ExPolygons &polygons, const Point &from)
{
    for (size_t idx = 0; idx < polygons.size(); ++idx) {
        if (polygons[idx].contains(from))
            return int(idx);
    }
    return -1;
}

//ORCA: Keep painted ear projection on the matching island when using EFC outline.
static bool closest_point_on_matching_island(const ExPolygons &raw_outline, const ExPolygons &efc_outline, const Point &from, Point &closest_out)
{
    const int island_idx = find_containing_expolygon_index(raw_outline, from);
    if (island_idx >= 0) {
        ExPolygons island_outline = intersection_ex(efc_outline, raw_outline[island_idx]);
        if (!island_outline.empty())
            return closest_point_on_expolygons(island_outline, from, closest_out);
    }
    return closest_point_on_expolygons(efc_outline, from, closest_out);
}

//ORCA: Use post-processed first-layer slices (including EFC) for brim outline.
// Returns ExPolygons of the bottom layer after all first-layer modifiers
// (including elephant foot compensation, if enabled) have been applied.
static ExPolygons get_print_object_bottom_layer_expolygons(const PrintObject &print_object)
{
    ExPolygons ex_polygons;
    for (LayerRegion *region : print_object.layers().front()->regions())
        Slic3r::append(ex_polygons, closing_ex(region->slices.surfaces, float(SCALED_EPSILON)));
    return ex_polygons;
}
//BBS adhesion coefficients from print object class
double getadhesionCoeff(const PrintObject* printObject)
{
    auto& insts = printObject->instances();
    auto objectVolumes = insts[0].model_instance->get_object()->volumes;

    auto print = printObject->print();
    std::vector<size_t> extrudersFirstLayer;
    auto firstLayerRegions = printObject->layers().front()->regions();
    if (!firstLayerRegions.empty()) {
        for (const LayerRegion* regionPtr : firstLayerRegions) {
            if (regionPtr->has_extrusions())
                extrudersFirstLayer.push_back(regionPtr->region().extruder(frExternalPerimeter));
        }
    }
    double adhesionCoeff = 1;
    for (const ModelVolume* modelVolume : objectVolumes) {
        if (modelVolume->is_precise_seam()) continue; // non-printing helper geometry
        for (auto iter = extrudersFirstLayer.begin(); iter != extrudersFirstLayer.end(); iter++) {
            if (modelVolume->extruder_id() == *iter) {
                if (Model::extruderParamsMap.find(modelVolume->extruder_id()) != Model::extruderParamsMap.end()) {
                    std::string filament_type = Model::extruderParamsMap.at(modelVolume->extruder_id()).materialName;
                    double adhesion_coefficient = 1.0; // Default value
                    MaterialType::get_adhesion_coefficient(filament_type, adhesion_coefficient);
                    adhesionCoeff = adhesion_coefficient;
                }
            }
        }
    }

    return adhesionCoeff;
   /*
   def->enum_values.push_back("PLA");
   def->enum_values.push_back("PET");
   def->enum_values.push_back("ABS");
   def->enum_values.push_back("ASA");
   def->enum_values.push_back("TPU");//BBS
   def->enum_values.push_back("FLEX");
   def->enum_values.push_back("HIPS");
   def->enum_values.push_back("EDGE");
   def->enum_values.push_back("NGEN");
   def->enum_values.push_back("NYLON");
   def->enum_values.push_back("PVA");
   def->enum_values.push_back("PC");
   def->enum_values.push_back("PP");
   def->enum_values.push_back("PEI");
   def->enum_values.push_back("PEEK");
   def->enum_values.push_back("PEKK");
   def->enum_values.push_back("POM");
   def->enum_values.push_back("PSU");
   def->enum_values.push_back("PVDF");
   def->enum_values.push_back("SCAFF");
   */
}

// BBS: second moment of area of a polygon
bool compSecondMoment(Polygon poly, Vec2d& sm)
{
    if (poly.is_clockwise())
        poly.make_counter_clockwise();

    sm = Vec2d(0., 0.);
    if (poly.points.size() >= 3) {
        Vec2d p1 = poly.points.back().cast<double>();
        for (const Point& p : poly.points) {
            Vec2d p2 = p.cast<double>();
            double a = cross2(p1, p2);

            sm += Vec2d((p1.y() * p1.y() + p1.y() * p2.y() + p2.y() * p2.y()), (p1.x() * p1.x() + p1.x() * p2.x() + p2.x() * p2.x())) * a / 12;
            p1 = p2;
        }
        return true;
    }
    return false;
}
// BBS: properties of an expolygon
struct ExPolyProp
{
    double aera = 0;
    Vec2d  centroid;
    Vec2d  secondMomentOfAreaRespectToCentroid;

};
// BBS: second moment of area of an expolyon
bool compSecondMoment(const ExPolygon& expoly, ExPolyProp& expolyProp)
{
    double aera = expoly.contour.area();
    Vec2d cent = expoly.contour.centroid().cast<double>() * aera;
    Vec2d sm;
    if (!compSecondMoment(expoly.contour, sm))
        return false;

    for (auto& hole : expoly.holes) {
        double a = hole.area();
        aera += hole.area();
        cent += hole.centroid().cast<double>() * a;
        Vec2d smh;
        if (compSecondMoment(hole, smh))
            sm += -smh;
    }

    cent = cent / aera;
    sm = sm - Vec2d(cent.y() * cent.y(), cent.x() * cent.x()) * aera;
    expolyProp.aera = aera;
    expolyProp.centroid = cent;
    expolyProp.secondMomentOfAreaRespectToCentroid = sm;
    return true;
}

// BBS: second moment of area of expolygons
bool compSecondMoment(const ExPolygons& expolys, double& smExpolysX, double& smExpolysY)
{
    if (expolys.empty()) return false;
    std::vector<ExPolyProp> props;
    for (const ExPolygon& expoly : expolys) {
        ExPolyProp prop;
        if (compSecondMoment(expoly, prop))
            props.push_back(prop);
    }
    if (props.empty())
        return false;
    double totalArea = 0.;
    Vec2d staticMoment(0., 0.);
    for (const ExPolyProp& prop : props) {
        totalArea += prop.aera;
        staticMoment += prop.centroid * prop.aera;
    }
    double totalCentroidX = staticMoment.x() / totalArea;
    double totalCentroidY = staticMoment.y() / totalArea;

    smExpolysX = 0;
    smExpolysY = 0;
    for (const ExPolyProp& prop : props) {
        double deltaX = prop.centroid.x() - totalCentroidX;
        double deltaY = prop.centroid.y() - totalCentroidY;
        smExpolysX += prop.secondMomentOfAreaRespectToCentroid.x() + prop.aera * deltaY * deltaY;
        smExpolysY += prop.secondMomentOfAreaRespectToCentroid.y() + prop.aera * deltaX * deltaX;
    }

    return true;
}
//BBS: config brimwidth by group of volumes
double configBrimWidthByVolumeGroups(double adhesion, double maxSpeed, const std::vector<ModelVolume*> modelVolumePtrs, const ExPolygons& expolys, double &groupHeight)
{
    // height of a group of volumes
    double height = 0;
    BoundingBoxf3 mergedBbx;
    for (const auto& modelVolumePtr : modelVolumePtrs) {
        if (modelVolumePtr->is_model_part()) {
            Slic3r::Transform3d t;
            if (modelVolumePtr->get_object()->instances.size() > 0)
                t = modelVolumePtr->get_object()->instances.front()->get_matrix() * modelVolumePtr->get_matrix();
            else
                t = modelVolumePtr->get_matrix();
            auto bbox = modelVolumePtr->mesh().transformed_bounding_box(t);
            mergedBbx.merge(bbox);
        }
    }
    auto bbox_size = mergedBbx.size();
    height = bbox_size(2);
    groupHeight = height;
    // second moment of the expolygons of the first layer of the volume group
    double Ixx = -1.e30, Iyy = -1.e30;
    if (!expolys.empty()) {
        if (!compSecondMoment(expolys, Ixx, Iyy))
            Ixx = Iyy = -1.e30;
    }
    Ixx = Ixx * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;
    Iyy = Iyy * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;

    // bounding box of the expolygons of the first layer of the volume
    BoundingBox bbox2;
    for (const auto& expoly : expolys)
        bbox2.merge(get_extents(expoly.contour));
    const double& bboxX = bbox2.size()(0);
    const double& bboxY = bbox2.size()(1);
    double thermalLength = sqrt(bboxX * bboxX + bboxY * bboxY) * SCALING_FACTOR;
    double thermalLengthRef = Model::getThermalLength(modelVolumePtrs);

    double height_to_area = std::max(height / Ixx * (bbox2.size()(1) * SCALING_FACTOR), height / Iyy * (bbox2.size()(0) * SCALING_FACTOR)) * height / 1920;
    double brim_width = adhesion * std::min(std::min(std::max(height_to_area * maxSpeed, thermalLength * 8. / thermalLengthRef * std::min(height, 30.) / 30.), 18.), 1.5 * thermalLength);
    // small brims are omitted
    if (brim_width < 5 && brim_width < 1.5 * thermalLength)
        brim_width = 0;
    // large brims are omitted
    if (brim_width > 18) brim_width = 18.;

    return brim_width;
}

// Generate ears
// Ported from SuperSlicer: https://github.com/supermerill/SuperSlicer/blob/45d0532845b63cd5cefe7de7dc4ef0e0ed7e030a/src/libslic3r/Brim.cpp#L1116
static ExPolygons make_brim_ears_auto(const ExPolygons& obj_expoly, coord_t size_ear, coord_t ear_detection_length,
                                      coordf_t brim_ears_max_angle, bool is_outer_brim) {
    ExPolygons mouse_ears_ex;
    if (size_ear <= 0) {
        return mouse_ears_ex;
    }
    // Detect places to put ears
    const coordf_t angle_threshold = (180 - brim_ears_max_angle) * PI / 180.0;
    Points pt_ears;
    for (const ExPolygon &poly : obj_expoly) {
        Polygon decimated_polygon = poly.contour;
        if (ear_detection_length > 0) {
            // decimate polygon
            Points points = poly.contour.points;
            points.push_back(points.front());
            points = MultiPoint::_douglas_peucker(points, ear_detection_length);
            if (points.size() > 4) { // don't decimate if it's going to be below 4 points, as it's surely enough to fill everything anyway
                points.erase(points.end() - 1);
                decimated_polygon.points = points;
            }
        }

        append(pt_ears, is_outer_brim ? decimated_polygon.convex_points(angle_threshold)
                                      : decimated_polygon.concave_points(angle_threshold));
    }

    // Then add ears
    // create ear pattern
    Polygon point_round;
    for (size_t i = 0; i < POLY_SIDE_COUNT; i++) {
        double angle = (2.0 * PI * i) / POLY_SIDE_COUNT;
        point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
    }

    // create ears
    for (Point &pt : pt_ears) {
        mouse_ears_ex.emplace_back();
        mouse_ears_ex.back().contour = point_round;
        mouse_ears_ex.back().contour.translate(pt);
    }

    return mouse_ears_ex;
}

static ExPolygons make_brim_ears(const PrintObject* object)
{
    ExPolygons mouse_ears_ex;
    BrimPoints brim_ear_points = object->model_object()->brim_points;
    if (brim_ear_points.size() <= 0) {
        return mouse_ears_ex;
    }
    //ORCA: Painted ears follow the EFC-adjusted outline when enabled, while
    // preserving their position along the selected outline segment.
    const bool use_efc_outline = use_brim_efc_outline(*object);
    const ExPolygons &raw_outline = object->layers().front()->lslices;
    //ORCA: Lazily computed EFC-adjusted bottom outline.
    //Stored separately so we can avoid recomputation unless EFC projection is used.
    ExPolygons efc_outline_storage;
    const ExPolygons* efc_outline = nullptr;

    const Geometry::Transformation& trsf = object->model_object()->instances[0]->get_transformation();
    Transform3d model_trsf = trsf.get_matrix_no_offset();
    const Point &center_offset = object->center_offset();
    model_trsf = model_trsf.pretranslate(Vec3d(- unscale<double>(center_offset.x()), - unscale<double>(center_offset.y()), 0));
    for (auto &pt : brim_ear_points) {
        Vec3f world_pos = pt.transform(trsf.get_matrix());
        if ( world_pos.z() > 0) continue;
        Polygon point_round;
        const coord_t size_ear = scale_(pt.head_front_radius);
        for (size_t i = 0; i < POLY_SIDE_COUNT; i++) {
            double angle = (2.0 * PI * i) / POLY_SIDE_COUNT;
            point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
        }
        mouse_ears_ex.emplace_back();
        mouse_ears_ex.back().contour = point_round;
        Vec3f pos = pt.transform(model_trsf);
        int32_t pt_x = scale_(pos.x());
        int32_t pt_y = scale_(pos.y());

        //ORCA: Project painted ears to the EFC-adjusted outline when enabled.
        if (use_efc_outline) {
            if (efc_outline == nullptr) {
                //ORCA: Compute the EFC-adjusted outline lazily for painted ear projection.
                efc_outline_storage = get_print_object_bottom_layer_expolygons(*object);
                efc_outline = &efc_outline_storage;
            }

            if (!efc_outline->empty()) {
                Point closest_point;
                //ORCA: Project within the matching island to avoid drifting to another island.
                if (closest_point_on_matching_island(
                        raw_outline,
                        *efc_outline,
                        Point(pt_x, pt_y),
                        closest_point)) {
                    pt_x = closest_point.x();
                    pt_y = closest_point.y();
                }
            }
        }

        mouse_ears_ex.back().contour.translate(Point(pt_x, pt_y));
    }
    return mouse_ears_ex;
}

static bool share_brim_bed_support(const Print &print, const PrintObject &object,
    unsigned int object_filament, const std::vector<unsigned int> &extruders)
{
    if (print.config().belt_printer.value || object.config().raft_layers.value > 0 ||
        object.support_layers().empty() || object.layers().empty() ||
        object.support_layers().front()->bottom_z() > EPSILON ||
        object.support_layers().front()->print_z > object.layers().front()->print_z + EPSILON || extruders.empty())
        return false;
    unsigned int support_filament = object.config().support_filament.value;
    if (support_filament == 0)
        support_filament = print.config().print_sequence == PrintSequence::ByObject ? object_filament : extruders.front() + 1;
    const unsigned int interface_filament = object.config().support_interface_filament.value;
    if (support_filament != object_filament || (interface_filament != 0 && interface_filament != object_filament))
        return false;
    bool planar = true;
    for_each_extrusion_path(object.support_layers().front()->support_fills, [&](const ExtrusionPath &path) {
        if (path.z_contoured)
            planar = false;
        for (const Point3 &point : path.polyline.points)
            if (point.z() != 0)
                planar = false;
    });
    return planar;
}

//BBS: create all brims
static ExPolygons outer_inner_brim_area(const Print& print,
    const float no_brim_offset, std::map<ObjectInstanceID, ExPolygons>& brimAreaMap,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec,
    std::vector<unsigned int>& printExtruders)
{
    unsigned int support_material_extruder = printExtruders.front() + 1;
    Flow flow = print.brim_flow();

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    std::map<ObjectInstanceID, ExPolygons> support_keepouts;
    std::set<ObjectID> shared_bed_support;
    Polygons   holes;

    struct brimWritten {
        bool obj;
        bool sup;
    };
    std::map<ObjectID, brimWritten> brimToWrite;
    for (const auto& objectWithExtruder : objPrintVec)
        brimToWrite.insert({ objectWithExtruder.first, {true,true} });

    ExPolygons objectIslands;
    for (unsigned int extruderNo : printExtruders) {
        ++extruderNo;
        for (const auto& objectWithExtruder : objPrintVec) {
            const PrintObject* object = print.get_object(objectWithExtruder.first);
            const BrimType     brim_type = object->config().brim_type.value;
            float              brim_offset = scale_(object->config().brim_object_gap.value);
            double             flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
            float              brim_width = scale_(floor(object->config().brim_width.value / flowWidth / 2) * flowWidth * 2);
            const float        scaled_flow_width = print.brim_flow().scaled_spacing();
            const float        scaled_additional_brim_width = scale_(floor(5 / flowWidth / 2) * flowWidth * 2);
            const float        scaled_half_min_adh_length = scale_(1.1);
            bool               has_brim_auto = object->config().brim_type == btAutoBrim;
            const bool         use_auto_brim_ears = object->config().brim_type == btEar;
            const bool         use_brim_ears = object->config().brim_type == btPainted;
            const bool         use_inner_brim_ears = (use_auto_brim_ears || use_brim_ears) && !object->config().brim_ears_outer_only.value;
            const bool         has_inner_brim = brim_type == btInnerOnly || brim_type == btOuterAndInner || use_inner_brim_ears;
            // btLeadingEdgeOnly is a belt-printer mode; on a flat bed there is no leading
            // edge, so it degrades to an ordinary outer brim rather than silently to none.
            const bool         has_outer_brim = brim_type == btOuterOnly || brim_type == btOuterAndInner || brim_type == btAutoBrim || brim_type == btLeadingEdgeOnly || use_auto_brim_ears || use_brim_ears;
            coord_t            ear_detection_length = scale_(object->config().brim_ears_detection_length.value);
            coordf_t           brim_ears_max_angle = object->config().brim_ears_max_angle.value;
            //ORCA: Select brim base slices from EFC-compensated outline when enabled.
            const bool         use_efc_outline = use_brim_efc_outline(*object);
            ExPolygons         brim_slices_storage;
            const ExPolygons*  brim_slices = nullptr;
            //ORCA: Select EFC-adjusted bottom outline when enabled.
            if (use_efc_outline)
                brim_slices_storage = get_print_object_bottom_layer_expolygons(*object);
            brim_slices = use_efc_outline ? &brim_slices_storage : &object->layers().front()->lslices;

            ExPolygons         brim_area_object;
            ExPolygons         no_brim_area_object;
            ExPolygons         no_brim_area_support;
            Polygons           holes_object;
            Polygons           holes_support;
            if (objectWithExtruder.second == extruderNo && brimToWrite.at(object->id()).obj) {
                double             adhesion = getadhesionCoeff(object);
                double             maxSpeed = Model::findMaxSpeed(object->model_object());
                // BBS: brims are generated by volume groups
                for (const auto& volumeGroup : object->firstLayerObjGroups()) {
                    // find volumePtrs included in this group
                    std::vector<ModelVolume*> groupVolumePtrs;
                    for (auto& volumeID : volumeGroup.volume_ids) {
                        ModelVolume* currentModelVolumePtr = nullptr;
                        //BBS: support shared object logic
                        const PrintObject* shared_object = object->get_shared_object();
                        if (!shared_object)
                            shared_object = object;
                        for (auto volumePtr : shared_object->model_object()->volumes) {
                            if (volumePtr->id() == volumeID) {
                                currentModelVolumePtr = volumePtr;
                                break;
                            }
                        }
                        if (currentModelVolumePtr != nullptr) groupVolumePtrs.push_back(currentModelVolumePtr);
                    }
                    if (groupVolumePtrs.empty()) continue;
                    double groupHeight = 0.;
                    // config brim width in auto-brim mode
                    if (has_brim_auto) {
                        double brimWidthRaw = configBrimWidthByVolumeGroups(adhesion, maxSpeed, groupVolumePtrs, volumeGroup.slices, groupHeight);
                        brim_width = scale_(floor(brimWidthRaw / flowWidth / 2) * flowWidth * 2);
                    }
                    ExPolygons volume_group_slices_efc;
                    const ExPolygons* volume_group_slices = &volumeGroup.slices;
                    if (use_efc_outline) {
                        //ORCA: When using EFC outline, restrict per-volume-group slices to the
                        // EFC-adjusted bottom footprint to keep brim width heuristics consistent.
                        volume_group_slices_efc = intersection_ex(*brim_slices, volumeGroup.slices);
                        volume_group_slices = &volume_group_slices_efc;
                    }
                    for (const ExPolygon& ex_poly : *volume_group_slices) {
                            // BBS: additional brim width will be added if part's adhesion area is too small and brim is not generated
                            float brim_width_mod;
                            if (brim_width < scale_(5.) && has_brim_auto && groupHeight > 10.) {
                                brim_width_mod = ex_poly.area() / ex_poly.contour.length() < scaled_half_min_adh_length
                                    && brim_width < scaled_flow_width ? brim_width + scaled_additional_brim_width : brim_width;
                            }
                            else {
                                brim_width_mod = brim_width;
                            }
                            //BBS: brim width should be limited to the 1.5*boundingboxSize of a single polygon.
                            if (has_brim_auto) {
                                BoundingBox bbox2 = ex_poly.contour.bounding_box();
                                brim_width_mod = std::min(brim_width_mod, float(std::max(bbox2.size()(0), bbox2.size()(1))));
                            }
                            brim_width_mod = floor(brim_width_mod / scaled_flow_width / 2) * scaled_flow_width * 2;

                            Polygons ex_poly_holes_reversed = ex_poly.holes;
                            polygons_reverse(ex_poly_holes_reversed);

                            if (has_outer_brim) {
                                // BBS: inner and outer boundary are offset from the same polygon incase of round off error.
                                auto innerExpoly = offset_ex(ex_poly.contour, brim_offset, jtRound, SCALED_RESOLUTION);
                                ExPolygons outerExpoly;
                                if (use_brim_ears) {
                                    outerExpoly = make_brim_ears(object);
                                    //outerExpoly = offset_ex(outerExpoly, brim_width_mod, jtRound, SCALED_RESOLUTION);
                                } else if (use_auto_brim_ears) {
                                    coord_t size_ear = (brim_width_mod - brim_offset - flow.scaled_spacing());
                                    outerExpoly = make_brim_ears_auto(innerExpoly, size_ear, ear_detection_length, brim_ears_max_angle, true);
                                }else {
                                    outerExpoly = offset_ex(innerExpoly, brim_width_mod, jtRound, SCALED_RESOLUTION);
                                }
                                append(brim_area_object, diff_ex(outerExpoly, innerExpoly));
                            }
                            if (has_inner_brim) {
                                ExPolygons outerExpoly;
                                auto innerExpoly = offset_ex(ex_poly_holes_reversed, -brim_width - brim_offset);
                                if (use_brim_ears) {
                                    outerExpoly = make_brim_ears(object);
                                } else if (use_auto_brim_ears) {
                                    coord_t size_ear = (brim_width - brim_offset - flow.scaled_spacing());
                                    outerExpoly = make_brim_ears_auto(offset_ex(ex_poly_holes_reversed, -brim_offset), size_ear, ear_detection_length, brim_ears_max_angle, false);
                                }else {
                                    outerExpoly = offset_ex(ex_poly_holes_reversed, -brim_offset);
                                }
                                append(brim_area_object, intersection_ex(diff_ex(outerExpoly, innerExpoly), ex_poly_holes_reversed));
                            }
                            if (!has_inner_brim) {
                                // BBS: brim should be apart from holes
                                append(no_brim_area_object, diff_ex(ex_poly_holes_reversed, offset_ex(ex_poly_holes_reversed, -no_brim_offset)));
                            }
                            if (!has_outer_brim)
                                append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly_holes_reversed));
                            append(holes_object, ex_poly_holes_reversed);
                        }
                    }
                auto objectIsland = offset_ex(*brim_slices, brim_offset, jtRound, SCALED_RESOLUTION);
                append(no_brim_area_object, objectIsland);

                brimToWrite.at(object->id()).obj = false;
                for (size_t instance_idx = 0; instance_idx < object->instances().size(); ++instance_idx) {
                    const PrintInstance& instance = object->instances()[instance_idx];
                    if (!brim_area_object.empty())
                        append_and_translate(brim_area_object, instance, instance_idx, brimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_object, instance);
                    append_and_translate(holes, holes_object, instance);
                    append_and_translate(objectIslands, objectIsland, instance);

                }
                for (const auto& [key, areas] : brimAreaMap)
                    if (key.object_id == object->id())
                        expolygons_append(brim_area, areas);
            }
            support_material_extruder = object->config().support_filament;
            if (support_material_extruder == 0 && object->has_support_material()) {
                if (print.config().print_sequence == PrintSequence::ByObject)
                    support_material_extruder = objectWithExtruder.second;
                else
                    support_material_extruder = printExtruders.front() + 1;
            }
            if (support_material_extruder == extruderNo && brimToWrite.at(object->id()).sup) {
                if (!object->support_layers().empty() && object->support_layers().front()->support_type==stInnerNormal) {
                    for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                        no_brim_area_support.emplace_back(support_contour);
                    }
                }
                // BBS
                if (!object->support_layers().empty() && object->support_layers().front()->support_type == stInnerTree) {
                    for (const ExPolygon &ex_poly : object->support_layers().front()->lslices) {
                        if (!has_outer_brim)
                            append(no_brim_area_support, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly.holes));
                        if (!has_inner_brim && !has_outer_brim)
                            append(no_brim_area_support, offset_ex(ex_poly.holes, -no_brim_offset));
                        append(holes_support, ex_poly.holes);
                        if (has_inner_brim || has_outer_brim)
                            append(no_brim_area_support, offset_ex(ex_poly.contour, 0));
                        no_brim_area_support.emplace_back(ex_poly.contour);
                    }
                }
                // Reserve physical beads of other instances/materials. Nominal
                // brim centers use half spacing and may be simplified.
                if (!object->support_layers().empty())
                    expolygons_append(no_brim_area_support, union_ex(
                        object->support_layers().front()->support_fills.polygons_covered_by_width()));
                const float brim_boundary_margin = 0.5f * float(flow.scaled_width() - flow.scaled_spacing()) +
                    float(SCALED_RESOLUTION + SCALED_EPSILON);
                no_brim_area_support = offset_ex(no_brim_area_support, brim_boundary_margin);
                brimToWrite.at(object->id()).sup = false;
                if (share_brim_bed_support(print, *object, objectWithExtruder.second, printExtruders))
                    shared_bed_support.insert(object->id());
                for (size_t instance_idx = 0; instance_idx < object->instances().size(); ++instance_idx) {
                    const PrintInstance &instance = object->instances()[instance_idx];
                    append_and_translate(no_brim_area_support, instance, instance_idx, support_keepouts);
                    append_and_translate(holes, holes_support, instance);
                }
            }
        }
    }

    int  extruder_nums = print.config().nozzle_diameter.values.size();
    std::vector<Polygons> extruder_unprintable_area = print.get_extruder_printable_polygons();
    // Orca: if per-extruder print area is not specified, use the whole bed as printable area for all extruders
    if (extruder_unprintable_area.empty()) {
        extruder_unprintable_area.resize(extruder_nums, Polygons{Model::getBedPolygon()});
    }
    std::vector<int> filament_map = print.get_filament_maps();

    if (print.has_wipe_tower() && !print.get_fake_wipe_tower().outer_wall.empty()) {
        ExPolygons expolyFromLines{};
        for (auto polyline : print.get_fake_wipe_tower().outer_wall.begin()->second) {
            polyline.remove_duplicate_points();
            expolyFromLines.emplace_back(polyline.points);
            expolyFromLines.back().translate(Point(scale_(print.get_fake_wipe_tower().pos[0]), scale_(print.get_fake_wipe_tower().pos[1])));
        }
        expolygons_append(no_brim_area, expolyFromLines);
    }

    for (const PrintObject* object : print.objects()) {
        ExPolygons extruder_no_brim_area = no_brim_area;
        auto iter = std::find_if(objPrintVec.begin(), objPrintVec.end(), [object](const std::pair<ObjectID, unsigned int>& item) {
            return item.first == object->id();
        });

        if (iter != objPrintVec.end()) {
            int extruder_id = filament_map[iter->second - 1] - 1;
            auto bedPoly = extruder_unprintable_area[extruder_id];
            auto bedExPoly   = diff_ex((offset(bedPoly, scale_(30.), jtRound, SCALED_RESOLUTION)), {bedPoly});
            if (!bedExPoly.empty()) {
                extruder_no_brim_area.push_back(bedExPoly.front());
            }
            //extruder_no_brim_area = offset2_ex(extruder_no_brim_area, scaled_flow_width, -scaled_flow_width); // connect scattered small areas to prevent generating very small brims

        }

        for (auto& [key, areas] : brimAreaMap)
            if (key.object_id == object->id()) {
                ExPolygons keepouts = extruder_no_brim_area;
                for (const auto &[support_key, support_area] : support_keepouts)
                    if (support_key != key || shared_bed_support.count(key.object_id) == 0)
                        expolygons_append(keepouts, support_area);
                areas = diff_ex(areas, keepouts);
            }

    }

    brim_area.clear();
    for (const PrintObject* object : print.objects()) {
        // BBS: brim should be contacted to at least one object's island or brim area
        for (auto map_it = brimAreaMap.begin(); map_it != brimAreaMap.end(); ++map_it) {
            if (map_it->first.object_id != object->id())
                continue;

            // find other objects' brim area
            ExPolygons otherExPolys;
            for (const auto& [other_key, other_areas] : brimAreaMap)
                if (other_key != map_it->first)
                    expolygons_append(otherExPolys, other_areas);

            auto tempArea = map_it->second;
            map_it->second.clear();

            for (int ia = 0; ia != tempArea.size(); ++ia) {
                // find this object's other brim area
                ExPolygons otherExPoly;
                for (int iao = 0; iao != tempArea.size(); ++iao)
                    if (iao != ia) otherExPoly.push_back(tempArea[iao]);

                auto offsetedTa = offset_ex(tempArea[ia], print.brim_flow().scaled_spacing() * 2, jtRound, SCALED_RESOLUTION);
                if (!intersection_ex(offsetedTa, objectIslands).empty() ||
                    !intersection_ex(offsetedTa, otherExPoly).empty() ||
                    !intersection_ex(offsetedTa, otherExPolys).empty())
                    map_it->second.push_back(tempArea[ia]);
            }
            expolygons_append(brim_area, map_it->second);
        }
    }
    return brim_area;
}
// Flip orientation of open polylines to minimize travel distance.
static void optimize_polylines_by_reversing(Polylines *polylines)
{
    for (size_t poly_idx = 1; poly_idx < polylines->size(); ++poly_idx) {
        const Polyline &prev = (*polylines)[poly_idx - 1];
        Polyline &      next = (*polylines)[poly_idx];

        if (!next.is_closed()) {
            double dist_to_start = (next.first_point() - prev.last_point()).cast<double>().norm();
            double dist_to_end   = (next.last_point() - prev.last_point()).cast<double>().norm();

            if (dist_to_end < dist_to_start)
                next.reverse();
        }
    }
}

static Polylines connect_brim_lines(Polylines &&polylines, const Polygons &brim_area, float max_connection_length)
{
    if (polylines.empty())
        return {};

    BoundingBox bbox = get_extents(polylines);
    bbox.merge(get_extents(brim_area));

    EdgeGrid::Grid grid(bbox.inflated(SCALED_EPSILON));
    grid.create(brim_area, polylines, coord_t(scale_(10.)));

    struct Visitor
    {
        explicit Visitor(const EdgeGrid::Grid &grid) : grid(grid) {}

        bool operator()(coord_t iy, coord_t ix)
        {
            // Called with a row and colum of the grid cell, which is intersected by a line.
            auto cell_data_range = grid.cell_data_range(iy, ix);
            this->intersect      = false;
            for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
                // End points of the line segment and their vector.
                auto segment = grid.segment(*it_contour_and_segment);
                if (Geometry::segments_intersect(segment.first, segment.second, brim_line.a, brim_line.b)) {
                    this->intersect = true;
                    return false;
                }
            }
            // Continue traversing the grid along the edge.
            return true;
        }

        const EdgeGrid::Grid &grid;
        Line                  brim_line;
        bool                  intersect = false;

    } visitor(grid);

    // Connect successive polylines if they are open, their ends are closer than max_connection_length.
    // Remove empty polylines.
    {
        // Skip initial empty lines.
        size_t poly_idx = 0;
        for (; poly_idx < polylines.size() && polylines[poly_idx].empty(); ++ poly_idx) ;
        size_t end = ++ poly_idx;
        double max_connection_length2 = Slic3r::sqr(max_connection_length);
        for (; poly_idx < polylines.size(); ++poly_idx) {
            Polyline &next = polylines[poly_idx];
            if (! next.empty()) {
                Polyline &prev = polylines[end - 1];
                bool   connect = false;
                if (! prev.is_closed() && ! next.is_closed()) {
                    double dist2 = (prev.last_point() - next.first_point()).cast<double>().squaredNorm();
                    if (dist2 <= max_connection_length2) {
                        visitor.brim_line.a = prev.last_point();
                        visitor.brim_line.b = next.first_point();
                        // Shrink the connection line to avoid collisions with the brim centerlines.
                        visitor.brim_line.extend(-SCALED_EPSILON);
                        grid.visit_cells_intersecting_line(visitor.brim_line.a, visitor.brim_line.b, visitor);
                        connect = ! visitor.intersect;
                    }
                }
                if (connect) {
                    append(prev.points, std::move(next.points));
                } else {
                    if (end < poly_idx)
                        polylines[end] = std::move(next);
                    ++ end;
                }
            }
        }
        if (end < polylines.size())
            polylines.erase(polylines.begin() + int(end), polylines.end());
    }

    return std::move(polylines);
}
//BBS: generate out brim by offseting ExPolygons 'islands_area_ex'
Polygons tryExPolygonOffset(const ExPolygons& islandAreaEx, const Print& print)
{
    const auto scaled_resolution = scaled<double>(print.config().resolution.value);
    Polygons   loops;
    ExPolygons islands_ex;
    Flow       flow = print.brim_flow();

    double resolution = 0.0125 / SCALING_FACTOR;
    islands_ex = islandAreaEx;
    for (ExPolygon& poly_ex : islands_ex)
        poly_ex.douglas_peucker(resolution);
    islands_ex = offset_ex(std::move(islands_ex), -0.5f * float(flow.scaled_spacing()), jtRound, resolution);
    for (size_t i = 0; !islands_ex.empty(); ++i) {
        for (ExPolygon& poly_ex : islands_ex)
            poly_ex.douglas_peucker(resolution);
        polygons_append(loops, to_polygons(islands_ex));
        islands_ex = offset_ex(std::move(islands_ex), -1.3f*float(flow.scaled_spacing()), jtRound, resolution);
        for (ExPolygon& poly_ex : islands_ex)
            poly_ex.douglas_peucker(resolution);
        islands_ex = offset_ex(std::move(islands_ex), 0.3f*float(flow.scaled_spacing()), jtRound, resolution);
    }
    return loops;
}
// Keep existing bed paths and fill only their unprinted, resolvable residue.
// The medial widths describe physical free space. variable_width() expects
// spacing widths, so compensate its rounded-extrusion correction first.
static void fill_bed_gaps(ExtrusionEntityCollection &brim, const ExPolygons &allowed, const Flow &flow,
    ExtrusionRole role = erBrim, double flow_ratio = 1.)
{
    const double correction = scale_(flow.height() * (1. - 0.25 * PI));
    // The rounded-rectangle flow model requires width >= layer height.
    const double minimum = std::max(0.25 * double(flow.scaled_width()), double(scale_(flow.height())));
    const double maximum = (role == erBrim ? 2. : 1.) * flow.scaled_width();
    for (int pass = 0; pass < 2; ++pass) {
        Polygons coverage;
        brim.polygons_covered_by_width(coverage, 1.f);
        const ExPolygons remaining = diff_ex(allowed, union_(coverage));
        const ExPolygons printable = opening_ex(remaining, float(0.5 * minimum));
        if (printable.empty())
            break;
        ThickPolylines medial;
        for (const ExPolygon &part : printable) {
            // The perimeter convenience wrapper prunes short branches. Those
            // branches are precisely the compact corner gaps needed here.
            Geometry::MedialAxis axis(minimum, maximum, part);
            axis.build(&medial);
        }
        ThickPolylines clipped;
        for (const ThickPolyline &line : medial) {
            ThickPolyline run;
            auto flush = [&] {
                if (run.is_valid())
                    clipped.push_back(std::move(run));
                run = ThickPolyline{};
            };
            for (size_t i = 1; i < line.points.size(); ++i) {
                const double a = std::min(line.width[2 * (i - 1)], maximum) - 4. * SCALED_EPSILON;
                const double b = std::min(line.width[2 * (i - 1) + 1], maximum) - 4. * SCALED_EPSILON;
                if (a < minimum && b < minimum) {
                    flush();
                    continue;
                }
                const double begin = a < minimum ? (minimum - a) / (b - a) : 0.;
                const double end = b < minimum ? (a - minimum) / (a - b) : 1.;
                const Vec2d p = line.points[i - 1].cast<double>();
                const Vec2d direction = (line.points[i] - line.points[i - 1]).cast<double>();
                const Point first = (p + begin * direction).cast<coord_t>();
                const Point last = (p + end * direction).cast<coord_t>();
                if (first == last)
                    continue;
                if (!run.empty() && run.last_point() != first)
                    flush();
                if (run.empty())
                    run.points.push_back(first);
                run.points.push_back(last);
                run.width.push_back(a + begin * (b - a) - correction);
                run.width.push_back(a + end * (b - a) - correction);
                if (end < 1.)
                    flush();
            }
            flush();
        }
        ExtrusionEntitiesPtr generated;
        variable_width(clipped, role, flow, generated);
        size_t added = 0;
        for (ExtrusionEntity *entity : generated) {
            for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                // Constant-width fragments must fit even where the skeleton
                // narrows. This also keeps rounded end caps out of keepouts.
                const ExPolygons centers = offset_ex(remaining, -0.5f * float(scale_(path.width)) - float(SCALED_EPSILON));
                for (Polyline &part : intersection_pl(Polylines{path.polyline.to_polyline()}, centers))
                    if (part.is_valid() && part.length() >= minimum) {
                        auto *fill = new ExtrusionPath(Polyline3(std::move(part)), path);
                        fill->mm3_per_mm *= flow_ratio;
                        brim.entities.push_back(fill);
                        ++added;
                    }
            });
            delete entity;
        }
        if (added == 0)
            break;
    }

    // A skeleton deliberately rejects non-facing edges, so compact corners
    // need concentric finishing. Use a physically valid narrow bead and keep
    // every centerline inset by its full radius, including rounded end caps.
    const Flow gap_flow = flow.with_width(unscale<float>(minimum));
    Polygons coverage;
    brim.polygons_covered_by_width(coverage, 1.f);
    const ExPolygons finishing_area = diff_ex(allowed,union_(coverage));
    ExPolygons centers = offset_ex(finishing_area,
        -0.5f * float(minimum) - float(SCALED_EPSILON));
    while (!centers.empty()) {
        ExPolygons contours;
        for (const ExPolygon &part : centers) {
            if (role == erSupportMaterialInterface && offset_ex(part,-0.5f*float(minimum)).empty()) {
                // A thin finishing island has no room for another perimeter.
                // Its boundary would print both sides of the same narrow strip
                // and leave an isolated loop. Restore the inset radius to the
                // medial widths, then keep each full bead within the free area.
                ThickPolylines medial;
                const size_t first_stroke = brim.entities.size();
                ExtrusionEntityCollection boundary;
                extrusion_entities_append_loops_and_paths(boundary.entities,to_polylines(to_polygons(ExPolygons{part})),
                    role,gap_flow.mm3_per_mm()*flow_ratio,gap_flow.width(),gap_flow.height());
                Geometry::MedialAxis axis(2.*SCALED_EPSILON,maximum,part);
                axis.build(&medial);
                for (ThickPolyline &line : medial)
                    for (double &width : line.width)
                        width += minimum-correction+2.*SCALED_EPSILON;
                ExtrusionEntitiesPtr strokes;
                variable_width(medial,role,flow,strokes);
                for (ExtrusionEntity *stroke : strokes) {
                    for_each_extrusion_path(*stroke,[&](const ExtrusionPath &original) {
                        ExtrusionPath path = original;
                        path.width=std::max(path.width,gap_flow.width());
                        auto fits = [&](float width) {
                            const Polygons bead = offset(path.polyline.to_polyline(),float(scale_(width*0.5f)),
                                jtRound,double(SCALED_EPSILON),etOpenRound);
                            return diff_ex(bead,finishing_area).empty();
                        };
                        if (!fits(path.width)) {
                            float low = gap_flow.width();
                            float high = path.width;
                            if (!fits(low))
                                return;
                            for (int step=0;step<10;++step) {
                                const float middle=0.5f*(low+high);
                                if (fits(middle)) low=middle;
                                else high=middle;
                            }
                            path.width=low;
                        }
                        path.mm3_per_mm=flow.with_width(path.width).mm3_per_mm()*flow_ratio;
                        brim.entities.push_back(new ExtrusionPath(std::move(path)));
                    });
                    delete stroke;
                }
                Polygons deposited;
                for (size_t i=first_stroke;i<brim.entities.size();++i)
                    brim.entities[i]->polygons_covered_by_width(deposited,1.f);
                const ExPolygons lost = opening_ex(diff_ex(boundary.polygons_covered_by_width(),deposited),
                    0.5f*float(minimum));
                // A compact round island may have no medial line. Retain its
                // original closed finishing stroke whenever replacing it would
                // remove a printable part of the foundation.
                if (area(lost) > double(SCALED_EPSILON)*SCALED_EPSILON) {
                    for (size_t i=first_stroke;i<brim.entities.size();++i)
                        delete brim.entities[i];
                    brim.entities.resize(first_stroke);
                    for (ExtrusionEntity *entity : boundary.entities)
                        brim.entities.push_back(entity);
                    boundary.entities.clear();
                }
            } else
                contours.push_back(part);
        }
        extrusion_entities_append_loops_and_paths(brim.entities, to_polylines(to_polygons(contours)),
            role,gap_flow.mm3_per_mm()*flow_ratio,gap_flow.width(),gap_flow.height());
        centers = offset_ex(contours,-float(gap_flow.scaled_spacing()));
    }
}

// Opt-in evidence from the actual allowed area, before G-code rounding.
static void trace_brim_coverage(const ExPolygons &allowed, const ExtrusionEntityCollection &brim,
    const Print &print, bool apply_plate_offset)
{
    const char *path = std::getenv("INLONG_BRIM_STAGE_TRACE");
    if (path == nullptr)
        return;
    static std::mutex mutex;
    const std::lock_guard<std::mutex> guard(mutex);
    std::ofstream out(path, std::ios::app);
    out << std::setprecision(15);
    const Vec3d origin = apply_plate_offset ? print.get_plate_origin() : Vec3d::Zero();
    auto polygons = [&](const Polygons &polys) {
        out << '[';
        for (size_t i = 0; i < polys.size(); ++i) {
            if (i) out << ',';
            out << '[';
            for (size_t j = 0; j < polys[i].points.size(); ++j) {
                if (j) out << ',';
                out << '[' << unscale<double>(polys[i].points[j].x()) + origin.x() << ','
                    << unscale<double>(polys[i].points[j].y()) + origin.y() << ']';
            }
            out << ']';
        }
        out << ']';
    };
    const Flow flow = print.brim_flow();
    out << "{\"width\":" << flow.width() << ",\"height\":" << flow.height()
        << ",\"spacing\":" << flow.spacing() << ",\"allowed\":";
    polygons(to_polygons(allowed));
    out << ",\"coverage\":";
    polygons(union_(brim.polygons_covered_by_width()));
    out << "}\n";
}

static ExtrusionEntityCollection makeBrimInfillImpl(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area, bool apply_plate_offset) {
    Polygons        loops = tryExPolygonOffset(singleBrimArea, print);
    Flow  flow = print.brim_flow();
    loops = union_pt_chained_outside_in(loops);

    std::vector<Polylines> loops_pl_by_levels;
    {
        Polylines              loops_pl = to_polylines(loops);
        loops_pl_by_levels.assign(loops_pl.size(), Polylines());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, loops_pl.size()),
            [&loops_pl_by_levels, &loops_pl /*, &islands_area*/](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    loops_pl_by_levels[i] = chain_polylines({ std::move(loops_pl[i]) });
                    //loops_pl_by_levels[i] = chain_polylines(intersection_pl({ std::move(loops_pl[i]) }, islands_area));
                }
            });
    }

    // output
    ExtrusionEntityCollection brim;
    // Reduce down to the ordered list of polylines.
    Polylines all_loops;
    for (Polylines& polylines : loops_pl_by_levels)
        append(all_loops, std::move(polylines));
    loops_pl_by_levels.clear();

    // Flip orientation of open polylines to minimize travel distance.
    optimize_polylines_by_reversing(&all_loops);
    all_loops = connect_brim_lines(std::move(all_loops), offset(singleBrimArea, float(SCALED_EPSILON)), float(flow.scaled_spacing()) * 2.f);

    extrusion_entities_append_loops_and_paths(brim.entities, std::move(all_loops), erBrim, float(flow.mm3_per_mm()), float(flow.width()), float(print.skirt_first_layer_height()));
    fill_bed_gaps(brim, singleBrimArea, flow);
    trace_brim_coverage(singleBrimArea, brim, print, apply_plate_offset);

    if (apply_plate_offset) {
        //BBS: finally apply the plate offset which may very large
        auto plate_offset = print.get_plate_origin();
        const Point3 shift(scaled(plate_offset.x()), scaled(plate_offset.y()), coord_t(0));
        for (ExtrusionEntity *entity : brim.entities) {
            if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity))
                for (ExtrusionPath &path : loop->paths)
                    path.polyline.translate(shift);
            else if (auto *path = dynamic_cast<ExtrusionPath *>(entity))
                path->polyline.translate(shift);
        }
    }

    return brim;
}

//BBS: a function creates the ExtrusionEntityCollection from the brim area defined by ExPolygons
ExtrusionEntityCollection makeBrimInfill(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area)
{
    return makeBrimInfillImpl(singleBrimArea, print, islands_area, true);
}

ExtrusionEntityCollection makeBrimInfillFromPlateCoordinates(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area)
{
    return makeBrimInfillImpl(singleBrimArea, print, islands_area, false);
}

static void splice_short_bed_paths(ExtrusionEntityCollection &result,
    const std::map<ExtrusionRole,ExPolygons> &allowed, bool full_width_only = false);

// A narrow projected contour may wrap around a clearance corner, so its
// straight medial road does not fit. Keep the best one-way arc of the original
// legal contour instead. The full bed coverage check below remains authoritative.
static Polylines single_bed_interface_arc(const Polyline &loop,const Flow &flow,const ExPolygons &centers)
{
    if (!loop.is_closed() || loop.points.size()<4 || loop.length()>4.*flow.scaled_width()) return {};
    const ExPolygon envelope(Geometry::convex_hull(loop.points));
    if (!offset_ex(envelope,-0.5f*float(flow.scaled_spacing())).empty()) return {};
    Points points=loop.points;
    points.pop_back();
    const auto span=get_extents(points).size();
    const int axis=span.x()>=span.y()?0:1;
    auto less=[axis](const Point &a,const Point &b) {
        return a[axis]<b[axis] || (a[axis]==b[axis] && a[1-axis]<b[1-axis]);
    };
    const size_t first=size_t(std::min_element(points.begin(),points.end(),less)-points.begin());
    const size_t last=size_t(std::max_element(points.begin(),points.end(),less)-points.begin());
    if (first==last) return {};
    const Polygons before=offset(loop,0.5f*float(flow.scaled_width()),jtRound,double(SCALED_EPSILON),etOpenRound);
    Polylines result;
    double best=std::numeric_limits<double>::max();
    for (const int direction:{1,-1}) {
        Points arc{points[first]};
        for (size_t i=first;i!=last;) {
            i=direction>0?(i+1)%points.size():(i+points.size()-1)%points.size();
            if (arc.back()!=points[i]) arc.push_back(points[i]);
        }
        Polyline candidate(std::move(arc));
        candidate.simplify(SCALED_EPSILON);
        if (!candidate.is_valid() || !diff_pl(Polylines{candidate},centers).empty()) continue;
        const Polygons after=offset(candidate,0.5f*float(flow.scaled_width()),jtRound,double(SCALED_EPSILON),etOpenRound);
        const double lost=area(diff_ex(before,after));
        if (lost<best) { best=lost; result=Polylines{std::move(candidate)}; }
    }
    return result;
}

// Replace narrow bed remnants by full process-width strokes. A larger bead
// covers the old stroke when its center moves by at most the radius increase.
// Treat the operation transactionally if a full-width route cannot fit.
bool normalize_bed_support_widths(ExtrusionEntityCollection &result,
    const ExtrusionEntityCollection &source,const Polygons &brim_coverage,const ExPolygons &support_regions)
{
    auto trace=[](bool success,const char *reason) {
        if (const char *file=std::getenv("INLONG_BED_WIDTH_TRACE")) {
            static std::mutex mutex;
            const std::lock_guard<std::mutex> guard(mutex);
            std::ofstream(file,std::ios::app)<<"{\"normalized\":"<<(success?"true":"false")
                <<",\"reason\":\""<<reason<<"\"}\n";
        }
    };
    auto fail=[&](const char *reason) { trace(false,reason); return false; };
    if (result.empty()) return true;
    if (support_regions.empty()) return fail("no_support_regions");
    const float tolerance=20.f*float(SCALED_EPSILON);
    using Key=std::pair<ExtrusionRole,float>;
    std::map<Key,float> widths;
    std::map<ExtrusionRole,Polygons> original;
    for_each_extrusion_path(source,[&](const ExtrusionPath &path) {
        if (!is_support(path.role())) return;
        auto &width=widths[{path.role(),path.height}];
        width=std::max(width,path.width);
        polygons_append(original[path.role()],offset(path.polyline.to_polyline(),float(scale_(path.width*0.5f)),
            jtRound,double(SCALED_EPSILON),etOpenRound));
    });
    struct Domain { ExPolygons centers; ExPolygons visible; ExPolygons material; std::map<Point,Point> projected; };
    std::map<Key,Domain> domains;
    for (const auto &[key,width]:widths) {
        Polygons forbidden=offset(brim_coverage,tolerance);
        for (const auto &[role,polygons]:original)
            if (role!=key.first) polygons_append(forbidden,polygons);
        const ExPolygons material=diff_ex(union_ex(support_regions,offset_ex(original.at(key.first),tolerance)),forbidden);
        auto &domain=domains[key];
        domain.material=material;
        domain.centers=offset_ex(material,-0.5f*float(scale_(width)),jtRound,double(SCALED_EPSILON));
        domain.visible=offset_ex(domain.centers,float(SCALED_EPSILON));
    }
    ExtrusionEntityCollection candidate=result;
    std::function<bool(ExtrusionEntity *&)> normalize=[&](ExtrusionEntity *&entity) {
        if (auto *collection=dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            for (auto *&child:collection->entities)
                if (!normalize(child)) return false;
            return true;
        }
        ExtrusionPaths paths;
        for_each_extrusion_path(*entity,[&](const ExtrusionPath &p) { paths.push_back(p); });
        if (paths.empty()) return true;
        ExtrusionPaths output;
        for (ExtrusionPath &path:paths) {
            if (!is_support(path.role())) return true;
            const Key key{path.role(),path.height};
            const auto width_it=widths.find(key);
            if (width_it==widths.end()) return fail("missing_process_flow");
            const float width=width_it->second;
            auto &domain=domains.at(key);
            if (domain.centers.empty()) return fail("empty_center_domain");
            if (path.height<=0.f) return fail("nonpositive_height");
            if (path.width+float(EPSILON)<path.height) return fail("subheight_flow");
            if (path.mm3_per_mm<=0.) return fail("nonpositive_flow");
            Points points;
            for (const Point3 &p3:path.polyline.points) {
                if (p3.z()!=0) return fail("nonplanar_path");
                const Point p=p3.to_point();
                auto it=domain.projected.find(p);
                if (it==domain.projected.end()) {
                    Point nearest=p;
                    bool inside=false;
                    double best=std::numeric_limits<double>::max();
                    for (const ExPolygon &part:domain.centers) {
                        if (part.contains(p)) { inside=true; break; }
                        const Point q=part.point_projection(p);
                        const double distance=(p-q).cast<double>().squaredNorm();
                        if (distance<best) { best=distance; nearest=q; }
                    }
                    it=domain.projected.emplace(p,inside?p:nearest).first;
                }
                const Point q=it->second;
                const double budget=0.5*scale_(std::max(0.f,width-path.width))+tolerance;
                if ((p-q).cast<double>().norm()>budget) return fail("center_move_budget");
                if (points.empty()) points.push_back(q);
                else if (!route_support_path(points.back(),q,domain.centers,domain.visible,2.*scale_(width),points)) return fail("no_continuous_center_route");
            }
            if (points.size()<2) continue;
            const double ratio=path.mm3_per_mm/(path.height*(path.width-path.height*(1.-0.25*PI)));
            path.width=width;
            path.mm3_per_mm=ratio*path.height*(width-path.height*(1.-0.25*PI));
            path.polyline=Polyline3(Polyline(std::move(points)));
            if (!output.empty() && output.back().last_point3()!=path.first_point3()) return fail("disconnected_entity");
            if (!output.empty() && output.back().last_point3()==path.first_point3() &&
                output.back().role()==path.role() && output.back().height==path.height && output.back().width==width &&
                std::abs(output.back().mm3_per_mm-path.mm3_per_mm)<0.000001) {
                output.back().polyline.points.insert(output.back().polyline.points.end(),
                    path.polyline.points.begin()+1,path.polyline.points.end());
            } else output.push_back(std::move(path));
        }
        if (output.empty()) return fail("empty_entity");
        bool replanned_interface=false;
        if (output.size()==1 && output.front().role()==erSupportMaterialInterface) {
            auto &path=output.front();
            const auto &domain=domains.at({path.role(),path.height});
            const Flow flow(path.width,path.height,0.f);
            Polylines center=support_interface_single_road(path.polyline.to_polyline(),domain.visible,
                flow.scaled_spacing(),flow.scaled_width());
            if (center.size()!=1)
                center=single_bed_interface_arc(path.polyline.to_polyline(),flow,domain.visible);
            if (center.size()==1) {
                path.polyline=Polyline3(std::move(center.front()));
                replanned_interface=true;
            }
        }
        auto *continuous=new ExtrusionMultiPath;
        continuous->paths=std::move(output);
        // A closed contour had no distinct entry/exit. Its new single road
        // may be oriented by the local connector before the final chain locks.
        if (!entity->can_reverse() && !replanned_interface) continuous->set_reverse();
        delete entity;
        entity=continuous;
        return true;
    };
    for (auto *&entity:candidate.entities)
        if (!normalize(entity)) return false;
    std::map<ExtrusionRole,ExPolygons> allowed;
    for (const auto &[key,domain]:domains)
        expolygons_append(allowed[key.first],domain.material);
    for (auto &[role,regions]:allowed)
        regions=union_ex(regions);
    // Width normalization can turn a previously valid narrow loop into two
    // overlapping return sides. Collapse it first, then splice only connectors
    // that fit at the full process width; do not reintroduce skinny transitions.
    splice_short_bed_paths(candidate,allowed,true);
    float minimum_height=std::numeric_limits<float>::max();
    for_each_extrusion_path(result,[&](const ExtrusionPath &p) { minimum_height=std::min(minimum_height,p.height); });
    const ExPolygons lost=opening_ex(diff_ex(result.polygons_covered_by_width(),candidate.polygons_covered_by_width()),
        0.5f*float(scale_(minimum_height)));
    if (area(lost)*SCALING_FACTOR*SCALING_FACTOR>0.002) return fail("printable_foundation_loss");
    result=std::move(candidate);
    trace(true,"full_process_widths");
    return true;
}

// Insert short strokes into a nearby continuous path without retracing a
// connector. The replaced receiver interval must retain printable coverage.
static void splice_short_bed_paths(ExtrusionEntityCollection &result,
    const std::map<ExtrusionRole,ExPolygons> &allowed, bool full_width_only)
{
    auto trace=[](const char *event,size_t index,size_t other,bool ok,double value) {
        if (const char *file=std::getenv("INLONG_BED_JOIN_TRACE")) {
            static std::mutex mutex;
            const std::lock_guard<std::mutex> guard(mutex);
            std::ofstream(file,std::ios::app)<<std::setprecision(12)
                <<"{\"event\":\""<<event<<"\",\"index\":"<<index<<",\"other\":"<<other
                <<",\"ok\":"<<(ok?"true":"false")<<",\"value\":"<<value<<"}\n";
        }
    };
    auto ratio = [](const ExtrusionPath &p) {
        return p.mm3_per_mm/(p.height*(p.width-p.height*(1.-0.25*PI)));
    };
    auto length = [](const ExtrusionPaths &paths) {
        double value=0.;
        for (const auto &p:paths) value+=p.length();
        return value;
    };
    auto slice = [](const ExtrusionPaths &paths,double begin,double end) {
        ExtrusionPaths output;
        double position=0.;
        for (const auto &p:paths) {
            const double size=p.length();
            const double a=std::max(0.,begin-position);
            const double b=std::min(size,end-position);
            if (b>a+SCALED_EPSILON) {
                ExtrusionPath part=p;
                part.polyline.clip_end(size-b);
                part.reverse();
                part.polyline.clip_end(a);
                part.reverse();
                output.push_back(std::move(part));
            }
            position+=size;
        }
        return output;
    };
    struct Projection { double distance; double position; };
    auto project = [](const Point3 &point,const ExtrusionPaths &paths) {
        Projection best{std::numeric_limits<double>::max(),0.};
        double position=0.;
        for (const auto &p:paths)
            for (size_t i=1;i<p.polyline.points.size();++i) {
                const Vec3d a=p.polyline.points[i-1].cast<double>();
                const Vec3d d=(p.polyline.points[i]-p.polyline.points[i-1]).cast<double>();
                const double size=d.norm();
                if (size<=SCALED_EPSILON) continue;
                const double t=std::clamp((point.cast<double>()-a).dot(d)/d.squaredNorm(),0.,1.);
                const double distance=(point.cast<double>()-a-t*d).norm();
                if (distance<best.distance) best={distance,position+t*size};
                position+=size;
            }
        return best;
    };
    auto connector = [&](const Point3 &a,const Point3 &b,const ExtrusionPath &flow,float maximum,
                         ExtrusionPaths &output) {
        if (a==b) return true;
        ExtrusionPath path(flow.role(),flow.mm3_per_mm,maximum,flow.height);
        path.polyline.points={a,b};
        // Normalization routes may lie on the one-epsilon visible boundary.
        // Use the same numerical allowance for a full-width seam connector;
        // otherwise its endpoint cap fails despite both existing roads fitting.
        const ExPolygons fit_domain=full_width_only ?
            offset_ex(allowed.at(path.role()),2.f*float(SCALED_EPSILON)) : allowed.at(path.role());
        auto fits = [&](float width) {
            return diff_ex(offset(path.polyline.to_polyline(),float(scale_(width*0.5f)),jtRound,
                double(SCALED_EPSILON),etOpenRound),fit_domain).empty();
        };
        if (full_width_only && std::getenv("INLONG_BED_JOIN_TRACE"))
            trace("connector_original_excess",0,0,fits(maximum),area(diff_ex(
                offset(path.polyline.to_polyline(),float(scale_(maximum*0.5f)),jtRound,
                    double(SCALED_EPSILON),etOpenRound),allowed.at(path.role())))*SCALING_FACTOR*SCALING_FACTOR);
        if (!fits(maximum) && full_width_only) {
            const ExPolygons centers=offset_ex(fit_domain,-0.5f*float(scale_(maximum)));
            const ExPolygons visible=offset_ex(centers,float(SCALED_EPSILON));
            Points points{a.to_point()};
            if (!route_support_path(a.to_point(),b.to_point(),centers,visible,2.*scale_(maximum),points)) return false;
            path.polyline=Polyline3(Polyline(std::move(points)));
            if (!fits(maximum)) return false;
        }
        if (!fits(maximum)) {
            float low=path.height,high=maximum;
            if (!fits(low)) return false;
            for (int step=0;step<10;++step) {
                const float middle=0.5f*(low+high);
                if (fits(middle)) low=middle;
                else high=middle;
            }
            path.width=low;
        }
        path.mm3_per_mm=ratio(flow)*path.height*(path.width-path.height*(1.-0.25*PI));
        output.push_back(std::move(path));
        return true;
    };
    std::vector<ExtrusionPaths> chains(result.entities.size());
    for (size_t i=0;i<chains.size();++i) {
        for_each_extrusion_path(*result.entities[i],[&](const ExtrusionPath &p) { chains[i].push_back(p); });
        bool valid=!chains[i].empty();
        for (size_t j=0;valid && j<chains[i].size();++j) {
            const auto &p=chains[i][j];
            valid=is_support(p.role()) && !p.z_contoured && p.height>0.f && p.width>=p.height && p.mm3_per_mm>0.;
            if (valid && j>0)
                valid=p.role()==chains[i][0].role() && p.height==chains[i][0].height &&
                    std::abs(ratio(p)-ratio(chains[i][0]))<0.00001 &&
                    chains[i][j-1].last_point3()==p.first_point3();
        }
        if (full_width_only && !chains[i].empty() && chains[i].front().role()==erSupportMaterialInterface) {
            trace("chain",i,chains[i].size(),valid,length(chains[i]));
            trace("reversible",i,0,result.entities[i]->can_reverse(),0.);
            for (size_t j=0;j<chains[i].size();++j) {
                trace("ratio",i,j,true,ratio(chains[i][j]));
                trace("height",i,j,true,chains[i][j].height);
            }
        }
        if (!valid) chains[i].clear();
    }
    for (size_t small=0;small<chains.size();++small) {
        if (chains[small].empty()) continue;
        const double small_length=length(chains[small]);
        const auto &first=chains[small].front();
        const auto &last=chains[small].back();
        float maximum=0.f;
        for (const auto &p:chains[small]) maximum=std::max(maximum,p.width);
        if (small_length>4.*scale_(maximum)) continue;
        // An open outline can have its two nearby ends on opposite sides of
        // a short finish. Their projected positions are 0 and total_length:
        // replacing that interval would erase the complete outline. Close
        // its seam by appending the finish instead, retaining every old road.
        if (full_width_only && first.role()==erSupportMaterialInterface &&
            first.first_point3()!=last.last_point3()) {
            struct EndCandidate { double distance; size_t index; bool reversed; };
            std::vector<EndCandidate> end_candidates;
            for (size_t receiver=0;receiver<chains.size();++receiver) {
                if (receiver==small || chains[receiver].empty()) continue;
                const auto &other=chains[receiver].front();
                const auto &tail=chains[receiver].back();
                if (other.role()!=first.role() || other.height!=first.height ||
                    length(chains[receiver])<=small_length+2.*scale_(maximum) ||
                    other.first_point3()==tail.last_point3() ||
                    std::abs(ratio(other)-ratio(first))>0.00001) continue;
                const double reach=2.*scale_(std::max(maximum,other.width));
                for (const bool reversed:{false,true}) {
                    if (reversed && !result.entities[small]->can_reverse()) continue;
                    const Point3 a=reversed?last.last_point3():first.first_point3();
                    const Point3 b=reversed?first.first_point3():last.last_point3();
                    const double da=(tail.last_point3()-a).cast<double>().norm();
                    const double db=(b-other.first_point3()).cast<double>().norm();
                    if (std::max(da,db)<=reach)
                        end_candidates.push_back({da+db,receiver,reversed});
                }
            }
            std::stable_sort(end_candidates.begin(),end_candidates.end(),[](const EndCandidate &a,const EndCandidate &b) {
                return std::tie(a.distance,a.index,a.reversed)<std::tie(b.distance,b.index,b.reversed);
            });
            trace("end_candidates",small,end_candidates.size(),true,small_length);
            bool closed=false;
            for (const auto &candidate:end_candidates) {
                ExtrusionPaths finish=chains[small];
                if (candidate.reversed) {
                    std::reverse(finish.begin(),finish.end());
                    for (auto &p:finish) p.reverse();
                }
                ExtrusionPaths proposed=chains[candidate.index];
                const bool first_fits=connector(proposed.back().last_point3(),finish.front().first_point3(),finish.front(),
                    std::min(proposed.back().width,finish.front().width),proposed);
                trace("end_entry",small,candidate.index,first_fits,candidate.distance);
                if (!first_fits) continue;
                proposed.insert(proposed.end(),finish.begin(),finish.end());
                const bool last_fits=connector(proposed.back().last_point3(),proposed.front().first_point3(),finish.back(),
                    std::min(finish.back().width,proposed.front().width),proposed);
                trace("end_exit",small,candidate.index,last_fits,candidate.distance);
                if (!last_fits) continue;
                auto *multi=new ExtrusionMultiPath;
                multi->paths=proposed;
                if (!result.entities[candidate.index]->can_reverse() || !result.entities[small]->can_reverse()) multi->set_reverse();
                delete result.entities[candidate.index];
                result.entities[candidate.index]=multi;
                delete result.entities[small];
                result.entities[small]=nullptr;
                chains[candidate.index]=std::move(proposed);
                chains[small].clear();
                closed=true;
                break;
            }
            if (closed) continue;
        }
        struct Candidate { double distance; size_t index; double a; double b; };
        std::vector<Candidate> candidates;
        for (size_t receiver=0;receiver<chains.size();++receiver) {
            if (receiver==small || chains[receiver].empty()) continue;
            const auto &other=chains[receiver].front();
            const double size=length(chains[receiver]);
            if (size<=small_length+2.*scale_(maximum) || other.role()!=first.role() ||
                other.height!=first.height || std::abs(ratio(other)-ratio(first))>0.00001) continue;
            const Projection a=project(first.first_point3(),chains[receiver]);
            const Projection b=project(last.last_point3(),chains[receiver]);
            const double reach=2.*scale_(std::max(maximum,other.width));
            if (std::max(a.distance,b.distance)>reach) continue;
            const double padding=0.125*scale_(std::min({maximum,other.width,first.height}));
            const double begin=std::max(0.,std::min(a.position,b.position)-padding);
            const double end=std::min(size,std::max(a.position,b.position)+padding);
            if (end-begin>small_length+reach) continue;
            candidates.push_back({a.distance+b.distance,receiver,begin,end});
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate &a,const Candidate &b) {
            return std::tie(a.distance,a.index,a.a,a.b)<std::tie(b.distance,b.index,b.a,b.b);
        });
        for (const auto &c:candidates) {
            const auto &receiver=chains[c.index];
            const double size=length(receiver);
            ExtrusionPaths prefix=slice(receiver,0.,c.a);
            ExtrusionPaths suffix=slice(receiver,c.b,size);
            const auto &left=prefix.empty()?receiver.front():prefix.back();
            const auto &right=suffix.empty()?receiver.back():suffix.front();
            const Point3 p=prefix.empty()?receiver.front().first_point3():left.last_point3();
            const Point3 q=suffix.empty()?receiver.back().last_point3():right.first_point3();
            ExtrusionPaths proposed=std::move(prefix);
            if (!connector(p,first.first_point3(),first,std::min(left.width,first.width),proposed)) continue;
            proposed.insert(proposed.end(),chains[small].begin(),chains[small].end());
            if (!connector(last.last_point3(),q,last,std::min(right.width,last.width),proposed)) continue;
            proposed.insert(proposed.end(),std::make_move_iterator(suffix.begin()),std::make_move_iterator(suffix.end()));
            Polygons before,after;
            for (const auto &path:receiver) path.polygons_covered_by_width(before,1.f);
            for (const auto &path:chains[small]) path.polygons_covered_by_width(before,1.f);
            for (const auto &path:proposed) path.polygons_covered_by_width(after,1.f);
            if (area(opening_ex(diff_ex(before,after),0.5f*float(scale_(first.height))))>
                double(SCALED_EPSILON)*SCALED_EPSILON) continue;
            auto *multi=new ExtrusionMultiPath;
            multi->paths=proposed;
            if (!result.entities[c.index]->can_reverse() || !result.entities[small]->can_reverse()) multi->set_reverse();
            delete result.entities[c.index];
            result.entities[c.index]=multi;
            delete result.entities[small];
            result.entities[small]=nullptr;
            chains[c.index]=std::move(proposed);
            chains[small].clear();
            break;
        }
    }
    result.entities.erase(std::remove(result.entities.begin(),result.entities.end(),nullptr),result.entities.end());
}

// Keep adjacent bed strokes in one ordered extrusion. A local connector must
// fit wholly in the planned support region and must not cross the final brim.
// Closed outlines remain intact. Compact finishing loops can be embedded as
// closed strokes in a chain; larger contours retain their original ordering.
void connect_bed_support_paths(ExtrusionEntityCollection &result,
    const ExtrusionEntityCollection &source, const Polygons &brim_coverage,
    const ExPolygons &support_regions)
{
    std::map<ExtrusionRole, Polygons> original;
    for_each_extrusion_path(source, [&](const ExtrusionPath &path) {
        path.polygons_covered_by_width(original[path.role()], 1.f);
    });
    std::map<ExtrusionRole, ExPolygons> allowed;
    for (const auto &[role, footprint] : original) {
        Polygons forbidden = offset(brim_coverage,float(SCALED_RESOLUTION));
        for (const auto &[other_role, other_footprint] : original)
            if (other_role != role)
                polygons_append(forbidden, other_footprint);
        allowed.emplace(role, support_regions.empty() ? diff_ex(footprint,forbidden) : diff_ex(support_regions,forbidden));
    }

    auto volume_ratio = [](const ExtrusionPath &path) {
        return path.mm3_per_mm / (path.height * (path.width - path.height * (1. - 0.25 * PI)));
    };
    auto reverse = [](ExtrusionPaths &paths) {
        std::reverse(paths.begin(), paths.end());
        for (ExtrusionPath &path : paths)
            path.reverse();
    };
    auto entities = std::move(result.entities);
    std::vector<ExtrusionPaths> runs(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        auto *path = dynamic_cast<ExtrusionPath *>(entities[i]);
        auto *loop = dynamic_cast<ExtrusionLoop *>(entities[i]);
        const bool compact_loop = loop != nullptr && loop->paths.size() == 1 &&
            loop->length() <= 4. * scale_(loop->paths.front().width);
        if (compact_loop)
            path = &loop->paths.front();
        if (path != nullptr && path->can_reverse() && is_support(path->role()) && !path->z_contoured &&
            path->height > 0.f && path->width >= path->height && path->mm3_per_mm > 0. &&
            (compact_loop || path->first_point3() != path->last_point3())) {
            runs[i].push_back(std::move(*path));
            delete entities[i];
            entities[i] = nullptr;
        }
    }
    for (size_t seed = 0; seed < runs.size(); ++seed) {
        if (runs[seed].empty())
            continue;
        ExtrusionPaths chain = std::move(runs[seed]);
        for (;;) {
            struct Candidate {
                double distance;
                size_t index;
                bool front;
                bool other_back;
            };
            std::vector<Candidate> candidates;
            for (size_t i = seed + 1; i < runs.size(); ++i) {
                if (runs[i].empty())
                    continue;
                for (bool front : {false, true}) {
                    const ExtrusionPath &a = front ? chain.front() : chain.back();
                    for (bool other_back : {false, true}) {
                        const ExtrusionPath &b = other_back ? runs[i].back() : runs[i].front();
                        if (a.role() != b.role() || a.height != b.height ||
                            std::abs(volume_ratio(a) - volume_ratio(b)) > 0.00001)
                            continue;
                        const Point3 &p = front ? a.first_point3() : a.last_point3();
                        const Point3 &q = other_back ? b.last_point3() : b.first_point3();
                        const double distance = (p - q).cast<double>().norm();
                        if (distance <= scale_(std::min(a.width, b.width)))
                            candidates.push_back({distance, i, front, other_back});
                    }
                }
            }
            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
                return std::tie(a.distance,a.index,a.front,a.other_back) < std::tie(b.distance,b.index,b.front,b.other_back);
            });
            bool joined = false;
            for (const Candidate &candidate : candidates) {
                const ExtrusionPath &a = candidate.front ? chain.front() : chain.back();
                const ExtrusionPath &b = candidate.other_back ? runs[candidate.index].back() : runs[candidate.index].front();
                const Point3 p = candidate.front ? a.first_point3() : a.last_point3();
                const Point3 q = candidate.other_back ? b.last_point3() : b.first_point3();
                const double ratio = volume_ratio(a);
                ExtrusionPath connector(a.role(), a.mm3_per_mm, std::min(a.width,b.width), a.height);
                connector.polyline.points = {p,q};
                auto fits = [&](float width) {
                    connector.width = width;
                    const Polygons bead = offset(connector.polyline.to_polyline(),float(scale_(0.5f*width)),
                        jtRound,DefaultLineMiterLimit,etOpenRound);
                    return diff_ex(bead, allowed.at(a.role())).empty();
                };
                if (p != q && !fits(connector.width)) {
                    float low = a.height;
                    float high = std::min(a.width,b.width);
                    if (!fits(low))
                        continue;
                    for (int step = 0; step < 10; ++step) {
                        const float middle = 0.5f * (low + high);
                        if (fits(middle)) low = middle;
                        else high = middle;
                    }
                    connector.width = low;
                }
                connector.mm3_per_mm = ratio * connector.height *
                    (connector.width - connector.height * (1. - 0.25 * PI));
                if (candidate.front)
                    reverse(chain);
                auto &other = runs[candidate.index];
                if (candidate.other_back)
                    reverse(other);
                if (p != q)
                    chain.push_back(std::move(connector));
                chain.insert(chain.end(), std::make_move_iterator(other.begin()), std::make_move_iterator(other.end()));
                other.clear();
                joined = true;
                break;
            }
            if (!joined)
                break;
        }
        if (chain.size() == 1) {
            if (chain.front().is_closed())
                entities[seed] = new ExtrusionLoop(std::move(chain));
            else
                entities[seed] = new ExtrusionPath(std::move(chain.front()));
        }
        else {
            auto *multi = new ExtrusionMultiPath;
            multi->paths = std::move(chain);
            entities[seed] = multi;
        }
    }
    for (ExtrusionEntity *entity : entities)
        if (entity != nullptr)
            result.entities.push_back(entity);
    splice_short_bed_paths(result,allowed);
}

ExtrusionEntityCollection support_outside_brim(const ExtrusionEntityCollection &support, const Polygons &brim_coverage,
    const ExPolygons &support_regions)
{
    ExtrusionEntityCollection result;
    result.no_sort = support.no_sort;
    if (!support.can_reverse())
        result.set_reverse();
    std::map<float, ExPolygons> keepouts;
    for (const ExtrusionEntity *entity : support.entities) {
        if (intersection_ex(entity->polygons_covered_by_width(), brim_coverage).empty()) {
            result.append(*entity);
            continue;
        }
        for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
            auto [it, inserted] = keepouts.try_emplace(path.width);
            if (inserted)
                it->second = offset_ex(brim_coverage, 0.5f * float(scale_(path.width)) + float(SCALED_EPSILON));
            for (Polyline &part : diff_pl(Polylines{path.polyline.to_polyline()}, it->second))
                if (part.is_valid() && part.length() > SCALED_EPSILON)
                    result.entities.push_back(new ExtrusionPath(Polyline3(std::move(part)), path));
        });
    }
    if (!support_regions.empty()) {
        struct RepairFlow {
            const ExtrusionPath *prototype { nullptr };
            Polygons original;
        };
        std::map<std::tuple<ExtrusionRole, float, float, double>, RepairFlow> repairs;
        for_each_extrusion_path(support, [&](const ExtrusionPath &path) {
            if (!is_support(path.role()))
                return;
            auto &repair = repairs[{path.role(),path.width,path.height,path.mm3_per_mm}];
            repair.prototype = &path;
            path.polygons_covered_by_width(repair.original, 1.f);
        });
        for (const auto &[_, repair] : repairs) {
            const ExtrusionPath &path = *repair.prototype;
            // A cut centerline may lose the outside part of its bead. Restore
            // only that previously printed material, with a width that fits;
            // do not grow a new support surface into the hollow tree or model.
            const ExPolygons required = diff_ex(repair.original,
                offset_ex(brim_coverage, float(SCALED_RESOLUTION)));
            const Flow flow = Flow::new_from_config_width(frSupportMaterial,
                ConfigOptionFloatOrPercent(path.width, false), path.width, path.height);
            fill_bed_gaps(result, required, flow, path.role(), path.mm3_per_mm / flow.mm3_per_mm());
        }
    }
    connect_bed_support_paths(result, support, brim_coverage, support_regions);
    return result;
}

void make_brim_support_overrides(const Print &print,
    const std::vector<std::pair<ObjectID, unsigned int>> &objects,
    const std::vector<unsigned int> &extruders,
    std::map<ObjectInstanceID, ExtrusionEntityCollection> &output)
{
    output.clear();
    for (const auto &[object_id, filament] : objects) {
        const PrintObject &object = *print.get_object(object_id);
        if (!share_brim_bed_support(print, object, filament, extruders))
            continue;
        for (size_t instance_idx = 0; instance_idx < object.instances().size(); ++instance_idx) {
            const ObjectInstanceID key{object_id, instance_idx};
            Polygons brim_coverage;
            for (const Print::SkirtBrimGroup &group : print.skirt_brim_groups())
                for (const Print::SkirtBrimGroup::Brim &brim : group.brims)
                    if (std::find(brim.instances.begin(), brim.instances.end(), key) != brim.instances.end())
                        brim.brim.polygons_covered_by_width(brim_coverage, 1.f);
            if (brim_coverage.empty())
                continue;
            brim_coverage = union_(brim_coverage);
            const Point shift = -object.instances()[instance_idx].shift;
            for (Polygon &polygon : brim_coverage)
                polygon.translate(shift);
            const ExtrusionEntityCollection &source = object.support_layers().front()->support_fills;
            if (!intersection_ex(source.polygons_covered_by_width(), brim_coverage).empty()) {
                const ExPolygons &regions=object.support_layers().front()->support_islands.empty() ?
                    object.support_layers().front()->lslices : object.support_layers().front()->support_islands;
                auto derived=support_outside_brim(source,brim_coverage,regions);
                auto original=derived;
                if (normalize_bed_support_widths(derived,source,brim_coverage,regions)) {
                    float minimum_height=std::numeric_limits<float>::max();
                    for_each_extrusion_path(source,[&](const ExtrusionPath &p) { minimum_height=std::min(minimum_height,p.height); });
                    const Polygons foundation=union_(derived.polygons_covered_by_width(),brim_coverage);
                    const ExPolygons lost=opening_ex(diff_ex(source.polygons_covered_by_width(),foundation),
                        0.5f*float(scale_(minimum_height)));
                    if (area(lost)*SCALING_FACTOR*SCALING_FACTOR>0.002) derived=std::move(original);
                }
                output.emplace(key,std::move(derived));
            }
        }
    }
}

//BBS: an overload of the orignal brim generator that generates the brim by obj and by extruders
void make_brim(const Print& print, PrintTryCancel try_cancel, Polygons& islands_area,
    std::map<ObjectID, ExtrusionEntityCollection>& brimMap,
    std::map<ObjectInstanceID, ExtrusionEntityCollection>& brimMapByInstance,
    std::vector<std::pair<ObjectID, unsigned int>> &objPrintVec,
    std::vector<unsigned int>& printExtruders,
    std::map<ObjectInstanceID, ExPolygons>* objectBrimAreasByInstanceOut)
{
    // Belt printers never use the flat plate brim.
    //
    // With a tilted belt the brim has to be laid onto the belt plane over many layers,
    // which BeltBrim.cpp does during posSupportMaterial.  With an untilted belt this
    // could in principle fall through and produce an ordinary brim, but it would never
    // reach the G-code: the plate brim is emitted out of skirt_brim_groups(), which
    // _make_skirt() builds, and that returns early for every belt printer.  Running the
    // generator anyway would just burn time on geometry nobody prints.
    if (print.config().belt_printer.value)
        return;

    std::map<ObjectInstanceID, ExPolygons> brimAreaMap;
    Flow                 flow = print.brim_flow();
    ExPolygons           islands_area_ex = outer_inner_brim_area(print,
        float(flow.scaled_spacing()), brimAreaMap, objPrintVec, printExtruders);

    if (!print.config().combine_brims) {
        ExPolygons claimed_area;
        for (auto& [_, areas] : brimAreaMap) {
            areas = diff_ex(areas, claimed_area);
            expolygons_append(claimed_area, areas);
        }
    }

    // BBS: Find boundingbox of the first layer
    for (const ObjectID printObjID : print.print_object_ids()) {
        BoundingBox bbx;
        PrintObject* object = const_cast<PrintObject*>(print.get_object(printObjID));
        //ORCA: Use EFC-compensated outline for brim bounding box when enabled.
        const ExPolygons brim_slices = use_brim_efc_outline(*object) ?
            get_print_object_bottom_layer_expolygons(*object) : object->layers().front()->lslices;
        for (const ExPolygon& ex_poly : brim_slices)
            for (const PrintInstance& instance : object->instances()) {
                auto ex_poly_translated = ex_poly;
                ex_poly_translated.translate(instance.shift_without_plate_offset());
                bbx.merge(get_extents(ex_poly_translated.contour));
            }
        if (!object->support_layers().empty())
        for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing())
            for (const PrintInstance& instance : object->instances()) {
                auto ex_poly_translated = support_contour;
                ex_poly_translated.translate(instance.shift_without_plate_offset());
                bbx.merge(get_extents(ex_poly_translated));
            }
        for (const auto& [key, areas] : brimAreaMap)
            if (key.object_id == printObjID)
                for (const ExPolygon& ex_poly : areas)
                    bbx.merge(get_extents(ex_poly.contour));
        object->firstLayerObjectBrimBoundingBox = bbx;
    }

    islands_area = to_polygons(islands_area_ex);

    // BBS: plate offset is applied
    const Vec3d plate_offset = print.get_plate_origin();
    Point plate_shift = Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
    for (size_t iia = 0; iia < islands_area.size(); ++iia)
        islands_area[iia].translate(plate_shift);

    // Orca: keep translated brim footprints for skirt grouping.
    auto translate_area_map = [plate_shift](const auto& src) {
        auto dst = src;
        for (auto& [_, areas] : dst)
            for (ExPolygon& area : areas)
                area.translate(plate_shift);
        return dst;
    };
    if (objectBrimAreasByInstanceOut != nullptr)
        *objectBrimAreasByInstanceOut = translate_area_map(brimAreaMap);

    // Orca: Generate brims per object instance. If Combine brims is enabled,
    // Print::_make_skirt() will join the touching ones.
    for (auto iter = brimAreaMap.begin(); iter != brimAreaMap.end(); ++iter) {
        if (!iter->second.empty()) {
            ExtrusionEntityCollection brim = makeBrimInfill(iter->second, print, islands_area);
            brimMap[iter->first.object_id].append(brim.entities);
            brimMapByInstance.emplace(iter->first, std::move(brim));
        };
    }
}
} // namespace Slic3r
