#ifndef slic3r_SupportLayer_hpp_
#define slic3r_SupportLayer_hpp_

#include <memory>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <oneapi/tbb/scalable_allocator.h>
#include <oneapi/tbb/spin_mutex.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
// for Slic3r::deque
#include "../libslic3r.h"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

// Support layer type to be used by SupportGeneratorLayer. This type carries a much more detailed information
// about the support layer type than the final support layers stored in a PrintObject.
enum class SupporLayerType {
	Unknown = 0,
	// Ratft base layer, to be printed with the support material.
	RaftBase,
	// Raft interface layer, to be printed with the support interface material. 
	RaftInterface,
	// Bottom contact layer placed over a top surface of an object. To be printed with a support interface material.
	BottomContact,
	// Dense interface layer, to be printed with the support interface material.
	// This layer is separated from an object by an BottomContact layer.
	BottomInterface,
	// Sparse base support layer, to be printed with a support material.
	Base,
	// Dense interface layer, to be printed with the support interface material.
	// This layer is separated from an object with TopContact layer.
	TopInterface,
	// Top contact layer directly supporting an overhang. To be printed with a support interface material.
	TopContact,
	// Some undecided type yet. It will turn into Base first, then it may turn into BottomInterface or TopInterface.
	Intermediate,
};

// A support layer type used internally by the SupportMaterial class. This class carries a much more detailed
// information about the support layer than the layers stored in the PrintObject, mainly
// the SupportGeneratorLayer is aware of the bridging flow and the interface gaps between the object and the support.
class SupportGeneratorLayer
{
public:
	void reset() {
		*this = SupportGeneratorLayer();
	}

	bool operator==(const SupportGeneratorLayer &layer2) const {
		return print_z == layer2.print_z && height == layer2.height && bridging == layer2.bridging;
	}

	// Order the layers by lexicographically by an increasing print_z and a decreasing layer height.
	bool operator<(const SupportGeneratorLayer &layer2) const {
		if (print_z < layer2.print_z) {
			return true;
		} else if (print_z == layer2.print_z) {
		 	if (height > layer2.height)
		 		return true;
		 	else if (height == layer2.height) {
		 		// Bridging layers first.
		 	 	return bridging && ! layer2.bridging;
		 	} else
		 		return false;
		} else
			return false;
	}

	void merge(SupportGeneratorLayer &&rhs) {
        // The union_() does not support move semantic yet, but maybe one day it will.
        this->polygons = union_(this->polygons, std::move(rhs.polygons));
        auto merge = [](std::unique_ptr<Polygons> &dst, std::unique_ptr<Polygons> &src) {
        	if (! dst || dst->empty())
        		dst = std::move(src);
        	else if (src && ! src->empty())
    			*dst = union_(*dst, std::move(*src));
        };
        merge(this->contact_polygons,  rhs.contact_polygons);
        merge(this->overhang_polygons, rhs.overhang_polygons);
        merge(this->enforcer_polygons, rhs.enforcer_polygons);
        rhs.reset();
    }

	// For the bridging flow, bottom_print_z will be above bottom_z to account for the vertical separation.
	// For the non-bridging flow, bottom_print_z will be equal to bottom_z.
	coordf_t bottom_print_z() const { return print_z - height; }

	// To sort the extremes of top / bottom interface layers.
	coordf_t extreme_z() const { return (this->layer_type == SupporLayerType::TopContact) ? this->bottom_z : this->print_z; }

	SupporLayerType layer_type { SupporLayerType::Unknown };
	// Z used for printing, in unscaled coordinates.
	coordf_t print_z { 0 };
	// Bottom Z of this layer. For soluble layers, bottom_z + height = print_z,
	// otherwise bottom_z + gap + height = print_z.
	coordf_t bottom_z { 0 };
	// Layer height in unscaled coordinates.
	coordf_t height { 0 };
	// Index of a PrintObject layer_id supported by this layer. This will be set for top contact layers.
	// If this is not a contact layer, it will be set to size_t(-1).
	size_t 	 idx_object_layer_above { size_t(-1) };
	// Index of a PrintObject layer_id, which supports this layer. This will be set for bottom contact layers.
	// If this is not a contact layer, it will be set to size_t(-1).
	size_t 	 idx_object_layer_below { size_t(-1) };
	// Use a bridging flow when printing this support layer.
	bool 	 bridging { false };

	// Polygons to be filled by the support pattern.
	Polygons polygons;
	// Currently for the contact layers only.
	std::unique_ptr<Polygons> contact_polygons;
	std::unique_ptr<Polygons> overhang_polygons;
	// Enforcers need to be propagated independently in case the "support on build plate only" option is enabled.
	std::unique_ptr<Polygons> enforcer_polygons;
};

// Layers are allocated and owned by a deque. Once a layer is allocated, it is maintained
// up to the end of a generate() method. The layer storage may be replaced by an allocator class in the future, 
// which would allocate layers by multiple chunks.
class SupportGeneratorLayerStorage {
public:
	SupportGeneratorLayer& allocate_unguarded(SupporLayerType layer_type) { 
		m_storage.emplace_back();
		m_storage.back().layer_type = layer_type;
	    return m_storage.back();
	}

	SupportGeneratorLayer& allocate(SupporLayerType layer_type)
	{ 
		m_mutex.lock();
		m_storage.emplace_back();
	    SupportGeneratorLayer *layer_new = &m_storage.back();
		m_mutex.unlock();
	    layer_new->layer_type = layer_type;
	    return *layer_new;
	}

private:
	template<typename BaseType>
	using Allocator = tbb::scalable_allocator<BaseType>;
	Slic3r::deque<SupportGeneratorLayer, Allocator<SupportGeneratorLayer>> 		m_storage;
	tbb::spin_mutex                         									m_mutex;
};
using SupportGeneratorLayersPtr		= std::vector<SupportGeneratorLayer*>;

// Optional disk-backed payload store for organic support source layers. The
// layer metadata remains resident so ordering and Z lookups stay cheap; only
// polygon payloads are paged in for the layer currently being converted to
// toolpaths.
class SupportGeneratorLayerPager
{
public:
    static constexpr uint32_t GroupCount = 5;

    explicit SupportGeneratorLayerPager(size_t layer_count) : m_layer_count(layer_count), m_offsets(GroupCount * layer_count, invalid_offset())
    {
        static std::atomic<uint64_t> next_id{ 0 };
        const uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
        const uint64_t stamp = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        m_path = std::filesystem::temp_directory_path() /
            ("inlong-support-layers-" + std::to_string(stamp) + "-" + std::to_string(id) + ".bin");
        m_output.open(m_path, std::ios::binary | std::ios::trunc);
        if (!m_output)
            throw std::runtime_error("Unable to create support layer page file.");
    }

    ~SupportGeneratorLayerPager()
    {
        m_output.close();
        m_input.close();
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    SupportGeneratorLayerPager(const SupportGeneratorLayerPager &) = delete;
    SupportGeneratorLayerPager &operator=(const SupportGeneratorLayerPager &) = delete;

    static void release_payload(SupportGeneratorLayer &layer)
    {
        Polygons().swap(layer.polygons);
        if (layer.contact_polygons)
            Polygons().swap(*layer.contact_polygons);
        if (layer.overhang_polygons)
            Polygons().swap(*layer.overhang_polygons);
        if (layer.enforcer_polygons)
            Polygons().swap(*layer.enforcer_polygons);
    }

    void spill(uint32_t group, size_t layer_idx, SupportGeneratorLayer &layer)
    {
        if (group >= GroupCount || layer_idx >= m_layer_count)
            throw std::runtime_error("Support layer page index out of range.");
        const uint64_t offset = uint64_t(m_output.tellp());
        m_offsets[size_t(group) * m_layer_count + layer_idx] = offset;
        write_layer(m_output, layer);
        if (!m_output)
            throw std::runtime_error("Unable to write support layer page file.");
    }

    void finish_write()
    {
        m_output.close();
        m_input.open(m_path, std::ios::binary);
        if (!m_input)
            throw std::runtime_error("Unable to open support layer page file.");
    }

    void load(uint32_t group, size_t layer_idx, SupportGeneratorLayer &layer)
    {
        if (group >= GroupCount || layer_idx >= m_layer_count)
            throw std::runtime_error("Support layer page index out of range.");
        const uint64_t offset = m_offsets[size_t(group) * m_layer_count + layer_idx];
        // Protect the whole hydrate transaction. A source layer can be
        // referenced by more than one output role, so releasing its payload
        // outside the read lock would race with another hydrate even when the
        // underlying file cursor itself is protected.
        const std::lock_guard<std::mutex> lock(m_input_mutex);
        release_payload(layer);
        if (offset == invalid_offset())
            return;
        // The toolpath planner may hydrate adjacent output layers in parallel.
        // std::ifstream keeps one shared cursor, so an unlocked seek/read can
        // interleave records and turn a valid page into an arbitrary polygon
        // count. On large jobs that manifests as bad allocation while the
        // corrupted count is reserved. Keep the disk read atomic; Clipper
        // processing remains outside this lock.
        m_input.clear();
        m_input.seekg(std::streamoff(offset));
        if (!m_input || !read_layer(m_input, layer))
            throw std::runtime_error("Unable to read support layer page file.");
    }

private:
    static constexpr uint64_t invalid_offset() { return std::numeric_limits<uint64_t>::max(); }

    static void write_u8(std::ofstream &stream, uint8_t value) { stream.write(reinterpret_cast<const char *>(&value), sizeof(value)); }
    static void write_u32(std::ofstream &stream, uint32_t value) { stream.write(reinterpret_cast<const char *>(&value), sizeof(value)); }
    static void write_u64(std::ofstream &stream, uint64_t value) { stream.write(reinterpret_cast<const char *>(&value), sizeof(value)); }
    static void write_i64(std::ofstream &stream, int64_t value) { stream.write(reinterpret_cast<const char *>(&value), sizeof(value)); }
    static void write_f64(std::ofstream &stream, double value) { stream.write(reinterpret_cast<const char *>(&value), sizeof(value)); }
    static bool read_u8(std::ifstream &stream, uint8_t &value) { return bool(stream.read(reinterpret_cast<char *>(&value), sizeof(value))); }
    static bool read_u32(std::ifstream &stream, uint32_t &value) { return bool(stream.read(reinterpret_cast<char *>(&value), sizeof(value))); }
    static bool read_u64(std::ifstream &stream, uint64_t &value) { return bool(stream.read(reinterpret_cast<char *>(&value), sizeof(value))); }
    static bool read_i64(std::ifstream &stream, int64_t &value) { return bool(stream.read(reinterpret_cast<char *>(&value), sizeof(value))); }
    static bool read_f64(std::ifstream &stream, double &value) { return bool(stream.read(reinterpret_cast<char *>(&value), sizeof(value))); }

    static void write_polygons(std::ofstream &stream, const Polygons &polygons)
    {
        write_u32(stream, uint32_t(polygons.size()));
        for (const Polygon &polygon : polygons) {
            write_u32(stream, uint32_t(polygon.size()));
            for (const Point &point : polygon.points) {
                write_i64(stream, int64_t(point.x()));
                write_i64(stream, int64_t(point.y()));
            }
        }
    }

    static bool read_polygons(std::ifstream &stream, Polygons &polygons)
    {
        uint32_t polygon_count = 0;
        if (!read_u32(stream, polygon_count))
            return false;
        polygons.clear();
        polygons.reserve(polygon_count);
        for (uint32_t polygon_idx = 0; polygon_idx < polygon_count; ++polygon_idx) {
            uint32_t point_count = 0;
            if (!read_u32(stream, point_count))
                return false;
            Polygon &polygon = polygons.emplace_back();
            polygon.points.reserve(point_count);
            for (uint32_t point_idx = 0; point_idx < point_count; ++point_idx) {
                int64_t x = 0, y = 0;
                if (!read_i64(stream, x) || !read_i64(stream, y))
                    return false;
                polygon.points.emplace_back(Point(coord_t(x), coord_t(y)));
            }
        }
        return true;
    }

    static void write_layer(std::ofstream &stream, const SupportGeneratorLayer &layer)
    {
        write_u32(stream, uint32_t(layer.layer_type));
        write_f64(stream, layer.print_z);
        write_f64(stream, layer.bottom_z);
        write_f64(stream, layer.height);
        write_u64(stream, uint64_t(layer.idx_object_layer_above));
        write_u64(stream, uint64_t(layer.idx_object_layer_below));
        write_u8(stream, layer.bridging ? 1 : 0);
        write_polygons(stream, layer.polygons);
        auto write_optional = [&stream](const std::unique_ptr<Polygons> &polygons) {
            write_u8(stream, polygons ? 1 : 0);
            if (polygons)
                write_polygons(stream, *polygons);
        };
        write_optional(layer.contact_polygons);
        write_optional(layer.overhang_polygons);
        write_optional(layer.enforcer_polygons);
    }

    static bool read_layer(std::ifstream &stream, SupportGeneratorLayer &layer)
    {
        uint32_t type = 0;
        uint8_t bridging = 0;
        uint64_t above = 0, below = 0;
        double print_z = 0., bottom_z = 0., height = 0.;
        if (!read_u32(stream, type) || !read_f64(stream, print_z) || !read_f64(stream, bottom_z) || !read_f64(stream, height) ||
            !read_u64(stream, above) || !read_u64(stream, below) || !read_u8(stream, bridging))
            return false;
        layer.layer_type = SupporLayerType(type);
        layer.print_z = coordf_t(print_z);
        layer.bottom_z = coordf_t(bottom_z);
        layer.height = coordf_t(height);
        layer.idx_object_layer_above = size_t(above);
        layer.idx_object_layer_below = size_t(below);
        layer.bridging = bridging != 0;
        if (!read_polygons(stream, layer.polygons))
            return false;
        auto read_optional = [&stream](std::unique_ptr<Polygons> &polygons) {
            uint8_t present = 0;
            if (!read_u8(stream, present))
                return false;
            if (present) {
                polygons = std::make_unique<Polygons>();
                return read_polygons(stream, *polygons);
            }
            polygons.reset();
            return true;
        };
        return read_optional(layer.contact_polygons) && read_optional(layer.overhang_polygons) && read_optional(layer.enforcer_polygons);
    }

    size_t m_layer_count{ 0 };
    std::filesystem::path m_path;
    std::ofstream m_output;
    std::ifstream m_input;
    std::mutex m_input_mutex;
    std::vector<uint64_t> m_offsets;
};

} // namespace Slic3r

#endif /* slic3r_SupportLayer_hpp_ */
