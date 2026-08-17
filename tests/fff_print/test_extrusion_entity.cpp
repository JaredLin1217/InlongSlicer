#include <catch2/catch_all.hpp>

#include <cstdlib>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;

static inline Slic3r::Point3 random_point3(float LO=-50, float HI=50)
{
    Vec3f pt = Vec3f(LO, LO, LO) + (Vec3d(rand(), rand(), rand()) * (HI-LO) / RAND_MAX).cast<float>();
	return Point3(pt.cast<coord_t>());
}


// build a sample extrusion entity collection with random start and end points.
static Slic3r::ExtrusionPath random_path(size_t length = 20, float LO = -50, float HI = 50)
{
    ExtrusionPath t {erPerimeter, 1.0, 1.0, 1.0};
    for (size_t j = 0; j < length; ++ j)
        t.polyline.append(random_point3(LO, HI));
    return t;
}

static Slic3r::ExtrusionPaths random_paths(size_t count = 10, size_t length = 20, float LO = -50, float HI = 50)
{
    Slic3r::ExtrusionPaths p;
    for (size_t i = 0; i < count; ++ i)
        p.push_back(random_path(length, LO, HI));
    return p;
}

SCENARIO("Polygon flattening", "[ExtrusionEntity]") {
    srand(0xDEADBEEF); // consistent seed for test reproducibility.

    // Generate one specific random path set and save it for later comparison
    Slic3r::ExtrusionPaths nosort_path_set = random_paths();

    Slic3r::ExtrusionEntityCollection sub_nosort;
    sub_nosort.append(nosort_path_set);
    sub_nosort.no_sort = true;

    Slic3r::ExtrusionEntityCollection sub_sort;
    sub_sort.no_sort = false;
    sub_sort.append(random_paths());

    GIVEN("A Extrusion Entity Collection with a child that has one child that is marked as no-sort") {
        Slic3r::ExtrusionEntityCollection sample;
        Slic3r::ExtrusionEntityCollection output;

        sample.append(sub_sort);
        sample.append(sub_nosort);
        sample.append(sub_sort);

        WHEN("The EEC is flattened with default options (preserve_order=false)") {
			output = sample.flatten();
            THEN("The output EEC contains no Extrusion Entity Collections") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 0);
            }
        }
        WHEN("The EEC is flattened with preservation (preserve_order=true)") {
			output = sample.flatten(true);
            THEN("The output EECs contains one EEC.") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 1);
            }
            AND_THEN("The ordered EEC contains the same order of elements than the original") {
                // find the entity in the collection
                for (auto e : output.entities)
                    if (e->is_collection()) {
                        ExtrusionEntityCollection *temp = dynamic_cast<ExtrusionEntityCollection*>(e);
                        // check each Extrusion path against nosort_path_set to see if the first and last match the same
                        CHECK(nosort_path_set.size() == temp->entities.size());
                        for (size_t i = 0; i < nosort_path_set.size(); ++ i) {
                            CHECK(temp->entities[i]->first_point() == nosort_path_set[i].first_point());
                            CHECK(temp->entities[i]->last_point() == nosort_path_set[i].last_point());
                        }
                    }
            }
        }
    }
}

SCENARIO("Loop clipping supports a negative seam overlap", "[ExtrusionEntity][SeamGap]") {
    ExtrusionPath path{erExternalPerimeter, 1.0, 0.4, 0.2};
    path.polyline = Polyline3(Points3{
        Point3(scale_(0.0),  scale_(0.0),  scale_(0.0)),
        Point3(scale_(10.0), scale_(0.0),  scale_(0.0)),
        Point3(scale_(10.0), scale_(10.0), scale_(0.0)),
        Point3(scale_(0.0),  scale_(10.0), scale_(0.0)),
        Point3(scale_(0.0),  scale_(0.0),  scale_(0.0)),
    });
    const ExtrusionLoop loop(path);
    const double        adjustment = scale_(2.0);

    SECTION("A positive distance still shortens the loop") {
        ExtrusionPaths clipped;
        loop.clip_end(adjustment, &clipped);

        REQUIRE(clipped.size() == 1);
        CHECK(clipped.front().length() == Catch::Approx(loop.length() - adjustment));
        CHECK(clipped.back().last_point3() == Point3(scale_(0.0), scale_(2.0), scale_(0.0)));
    }

    SECTION("A negative distance appends the beginning of the loop") {
        ExtrusionPaths overlapped;
        loop.clip_end(-adjustment, &overlapped);

        REQUIRE(overlapped.size() == 2);
        CHECK(overlapped.front().length() == Catch::Approx(loop.length()));
        CHECK(overlapped.back().length() == Catch::Approx(adjustment));
        CHECK(overlapped.front().last_point3() == overlapped.back().first_point3());
        CHECK(overlapped.back().last_point3() == Point3(scale_(2.0), scale_(0.0), scale_(0.0)));
    }

    SECTION("An excessive negative distance is limited to one extra loop") {
        ExtrusionPaths overlapped;
        loop.clip_end(-2.0 * loop.length(), &overlapped);

        REQUIRE(overlapped.size() == 2);
        CHECK(overlapped.front().length() + overlapped.back().length() == Catch::Approx(2.0 * loop.length()));
    }
}
