#include "mvt_decoder.hpp"

#include "duckdb/common/exception.hpp"
#include "miniz_wrapper.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Gzip
//===--------------------------------------------------------------------===//

string MaybeGunzipTile(const_data_ptr_t data, idx_t size) {
	// Not gzip - the .mbtiles spec allows raw tiles, so pass it through.
	if (size < 18 || data[0] != 0x1F || data[1] != 0x8B) {
		return string(const_char_ptr_cast(data), size);
	}
	// A gzip member ends with the uncompressed length modulo 2^32. Tiles are
	// orders of magnitude below that, so this is exact here.
	uint32_t uncompressed_size;
	memcpy(&uncompressed_size, data + size - 4, sizeof(uncompressed_size));
	if (uncompressed_size == 0) {
		return string();
	}

	string result;
	result.resize(uncompressed_size);
	MiniZStream stream;
	try {
		stream.Decompress(const_char_ptr_cast(data), size, &result[0], uncompressed_size);
	} catch (const std::exception &error) {
		throw InvalidInputException("Failed to decompress vector tile: %s", error.what());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Protobuf wire format
//===--------------------------------------------------------------------===//

namespace {

enum class WireType : uint8_t { VARINT = 0, FIXED64 = 1, LENGTH_DELIMITED = 2, FIXED32 = 5 };

//! Minimal reader over the subset of protobuf that vector tiles use.
struct ProtoReader {
	ProtoReader(const_data_ptr_t data, idx_t size) : ptr(data), end(data + size) {
	}

	const_data_ptr_t ptr;
	const_data_ptr_t end;

	bool Done() const {
		return ptr >= end;
	}

	void Require(idx_t bytes) const {
		if (static_cast<idx_t>(end - ptr) < bytes) {
			throw InvalidInputException("Malformed vector tile: truncated field");
		}
	}

	uint64_t ReadVarint() {
		uint64_t result = 0;
		uint32_t shift = 0;
		while (true) {
			Require(1);
			const uint8_t byte = *ptr++;
			if (shift > 63) {
				throw InvalidInputException("Malformed vector tile: oversized varint");
			}
			result |= static_cast<uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) {
				break;
			}
			shift += 7;
		}
		return result;
	}

	//! Reads a tag, returning the field number and setting `type`.
	uint32_t ReadTag(WireType &type) {
		const auto tag = ReadVarint();
		type = static_cast<WireType>(tag & 0x7);
		return static_cast<uint32_t>(tag >> 3);
	}

	//! Returns a reader over the next length-delimited field.
	ProtoReader ReadSubMessage() {
		const auto length = ReadVarint();
		Require(length);
		ProtoReader sub(ptr, length);
		ptr += length;
		return sub;
	}

	string ReadString() {
		const auto length = ReadVarint();
		Require(length);
		string result(const_char_ptr_cast(ptr), length);
		ptr += length;
		return result;
	}

	double ReadDouble() {
		Require(sizeof(double));
		double value;
		memcpy(&value, ptr, sizeof(value));
		ptr += sizeof(value);
		return value;
	}

	float ReadFloat() {
		Require(sizeof(float));
		float value;
		memcpy(&value, ptr, sizeof(value));
		ptr += sizeof(value);
		return value;
	}

	//! Skips a field we do not care about, so unknown fields stay harmless.
	void SkipField(WireType type) {
		switch (type) {
		case WireType::VARINT:
			ReadVarint();
			return;
		case WireType::FIXED64:
			Require(8);
			ptr += 8;
			return;
		case WireType::LENGTH_DELIMITED: {
			const auto length = ReadVarint();
			Require(length);
			ptr += length;
			return;
		}
		case WireType::FIXED32:
			Require(4);
			ptr += 4;
			return;
		default:
			throw InvalidInputException("Malformed vector tile: unknown wire type %d", static_cast<int>(type));
		}
	}
};

int64_t ZigZagDecode(uint64_t value) {
	return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

//===--------------------------------------------------------------------===//
// Geometry
//===--------------------------------------------------------------------===//

enum class MVTGeometryType : uint8_t { UNKNOWN = 0, POINT = 1, LINESTRING = 2, POLYGON = 3 };

struct Coordinate {
	double x;
	double y;
};

//! Converts a tile-local position into longitude/latitude by inverting the
//! Web Mercator projection for this tile's address.
struct TileProjection {
	TileProjection(int32_t zoom, int64_t tile_x, int64_t tile_y, uint32_t extent)
	    : tile_x(static_cast<double>(tile_x)), tile_y(static_cast<double>(tile_y)),
	      scale(static_cast<double>(uint64_t(1) << zoom)), extent(static_cast<double>(extent)) {
	}

	double tile_x;
	double tile_y;
	double scale;
	double extent;

	Coordinate Project(int64_t x, int64_t y) const {
		const double world_x = tile_x + static_cast<double>(x) / extent;
		const double world_y = tile_y + static_cast<double>(y) / extent;
		const double longitude = world_x / scale * 360.0 - 180.0;
		const double n = M_PI * (1.0 - 2.0 * world_y / scale);
		const double latitude = 180.0 / M_PI * std::atan(std::sinh(n));
		return Coordinate {longitude, latitude};
	}
};

//! A ring's winding order tells exterior from interior in MVT v2. Positive
//! area (in tile coordinates, y growing downward) means an exterior ring.
double SignedArea(const vector<Coordinate> &ring) {
	double total = 0;
	for (idx_t i = 0; i < ring.size(); i++) {
		const auto &a = ring[i];
		const auto &b = ring[(i + 1) % ring.size()];
		total += a.x * b.y - b.x * a.y;
	}
	return total / 2.0;
}

//===--------------------------------------------------------------------===//
// WKB output
//===--------------------------------------------------------------------===//

void AppendUInt32(string &out, uint32_t value) {
	out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void AppendDouble(string &out, double value) {
	out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

//! Writes a little-endian WKB header. DuckDB's GEOMETRY is WKB internally, so
//! what this produces can be handed straight to a GEOMETRY vector.
void AppendWKBHeader(string &out, uint32_t geometry_type) {
	out += static_cast<char>(1); // little endian
	AppendUInt32(out, geometry_type);
}

void AppendPoint(string &out, const Coordinate &point) {
	AppendDouble(out, point.x);
	AppendDouble(out, point.y);
}

void AppendRing(string &out, const vector<Coordinate> &ring, bool close) {
	// WKB polygon rings must repeat their first point; MVT leaves it implicit.
	const bool needs_closing =
	    close && !ring.empty() && (ring.front().x != ring.back().x || ring.front().y != ring.back().y);
	AppendUInt32(out, static_cast<uint32_t>(ring.size() + (needs_closing ? 1 : 0)));
	for (auto &point : ring) {
		AppendPoint(out, point);
	}
	if (needs_closing) {
		AppendPoint(out, ring.front());
	}
}

constexpr uint32_t WKB_POINT = 1;
constexpr uint32_t WKB_LINESTRING = 2;
constexpr uint32_t WKB_POLYGON = 3;
constexpr uint32_t WKB_MULTIPOINT = 4;
constexpr uint32_t WKB_MULTILINESTRING = 5;
constexpr uint32_t WKB_MULTIPOLYGON = 6;

//! Decodes the command stream into rings of projected coordinates.
//!
//! MVT geometry is a flat list of commands - MoveTo starts a new part, LineTo
//! extends it, ClosePath ends a ring - with all coordinates delta-encoded from
//! the previous point.
vector<vector<Coordinate>> DecodeGeometryParts(ProtoReader &reader, const TileProjection &projection) {
	vector<vector<Coordinate>> parts;
	vector<Coordinate> current;
	int64_t cursor_x = 0;
	int64_t cursor_y = 0;

	while (!reader.Done()) {
		const auto command_integer = reader.ReadVarint();
		const auto command = command_integer & 0x7;
		const auto count = command_integer >> 3;

		switch (command) {
		case 1: // MoveTo - begins a new part
			for (uint64_t i = 0; i < count; i++) {
				if (!current.empty()) {
					parts.push_back(std::move(current));
					current.clear();
				}
				cursor_x += ZigZagDecode(reader.ReadVarint());
				cursor_y += ZigZagDecode(reader.ReadVarint());
				current.push_back(projection.Project(cursor_x, cursor_y));
			}
			break;
		case 2: // LineTo - extends the current part
			for (uint64_t i = 0; i < count; i++) {
				cursor_x += ZigZagDecode(reader.ReadVarint());
				cursor_y += ZigZagDecode(reader.ReadVarint());
				current.push_back(projection.Project(cursor_x, cursor_y));
			}
			break;
		case 7: // ClosePath - takes no parameters
			if (!current.empty()) {
				parts.push_back(std::move(current));
				current.clear();
			}
			break;
		default:
			throw InvalidInputException("Malformed vector tile: unknown geometry command %llu",
			                            static_cast<unsigned long long>(command));
		}
	}
	if (!current.empty()) {
		parts.push_back(std::move(current));
	}
	return parts;
}

string BuildWKB(MVTGeometryType type, const vector<vector<Coordinate>> &parts) {
	string wkb;
	switch (type) {
	case MVTGeometryType::POINT: {
		// Every MoveTo produced its own single-point part.
		idx_t total = 0;
		for (auto &part : parts) {
			total += part.size();
		}
		if (total == 1) {
			AppendWKBHeader(wkb, WKB_POINT);
			AppendPoint(wkb, parts[0][0]);
		} else {
			AppendWKBHeader(wkb, WKB_MULTIPOINT);
			AppendUInt32(wkb, static_cast<uint32_t>(total));
			for (auto &part : parts) {
				for (auto &point : part) {
					AppendWKBHeader(wkb, WKB_POINT);
					AppendPoint(wkb, point);
				}
			}
		}
		return wkb;
	}
	case MVTGeometryType::LINESTRING: {
		if (parts.size() == 1) {
			AppendWKBHeader(wkb, WKB_LINESTRING);
			AppendRing(wkb, parts[0], false);
		} else {
			AppendWKBHeader(wkb, WKB_MULTILINESTRING);
			AppendUInt32(wkb, static_cast<uint32_t>(parts.size()));
			for (auto &part : parts) {
				AppendWKBHeader(wkb, WKB_LINESTRING);
				AppendRing(wkb, part, false);
			}
		}
		return wkb;
	}
	case MVTGeometryType::POLYGON: {
		// Group rings into polygons: an exterior ring starts a new polygon and
		// the interior rings that follow belong to it.
		vector<vector<const vector<Coordinate> *>> polygons;
		for (auto &ring : parts) {
			if (ring.size() < 3) {
				continue;
			}
			const bool exterior = SignedArea(ring) > 0;
			if (exterior || polygons.empty()) {
				polygons.emplace_back();
			}
			polygons.back().push_back(&ring);
		}
		if (polygons.empty()) {
			AppendWKBHeader(wkb, WKB_POLYGON);
			AppendUInt32(wkb, 0);
			return wkb;
		}
		if (polygons.size() == 1) {
			AppendWKBHeader(wkb, WKB_POLYGON);
			AppendUInt32(wkb, static_cast<uint32_t>(polygons[0].size()));
			for (auto *ring : polygons[0]) {
				AppendRing(wkb, *ring, true);
			}
		} else {
			AppendWKBHeader(wkb, WKB_MULTIPOLYGON);
			AppendUInt32(wkb, static_cast<uint32_t>(polygons.size()));
			for (auto &polygon : polygons) {
				AppendWKBHeader(wkb, WKB_POLYGON);
				AppendUInt32(wkb, static_cast<uint32_t>(polygon.size()));
				for (auto *ring : polygon) {
					AppendRing(wkb, *ring, true);
				}
			}
		}
		return wkb;
	}
	default:
		return wkb;
	}
}

//===--------------------------------------------------------------------===//
// Layer decoding
//===--------------------------------------------------------------------===//

Value DecodeValue(ProtoReader reader) {
	while (!reader.Done()) {
		WireType type;
		const auto field = reader.ReadTag(type);
		switch (field) {
		case 1:
			return Value(reader.ReadString());
		case 2:
			return Value::DOUBLE(static_cast<double>(reader.ReadFloat()));
		case 3:
			return Value::DOUBLE(reader.ReadDouble());
		case 4:
			return Value::BIGINT(static_cast<int64_t>(reader.ReadVarint()));
		case 5:
			return Value::UBIGINT(reader.ReadVarint());
		case 6:
			return Value::BIGINT(ZigZagDecode(reader.ReadVarint()));
		case 7:
			return Value::BOOLEAN(reader.ReadVarint() != 0);
		default:
			reader.SkipField(type);
			break;
		}
	}
	return Value();
}

MVTLayer DecodeLayer(ProtoReader reader, int32_t zoom, int64_t tile_x, int64_t tile_y) {
	MVTLayer layer;
	vector<string> keys;
	vector<Value> values;
	// Features are decoded only after the whole layer is read, because the key
	// and value tables they index into may appear after them.
	vector<pair<vector<uint32_t>, pair<MVTGeometryType, vector<uint8_t>>>> pending;

	while (!reader.Done()) {
		WireType type;
		const auto field = reader.ReadTag(type);
		switch (field) {
		case 1:
			layer.name = reader.ReadString();
			break;
		case 5:
			layer.extent = static_cast<uint32_t>(reader.ReadVarint());
			break;
		case 3:
			keys.push_back(reader.ReadString());
			break;
		case 4:
			values.push_back(DecodeValue(reader.ReadSubMessage()));
			break;
		case 2: {
			auto feature_reader = reader.ReadSubMessage();
			vector<uint32_t> tags;
			auto geometry_type = MVTGeometryType::UNKNOWN;
			vector<uint8_t> geometry_bytes;
			bool has_id = false;
			uint64_t id = 0;

			while (!feature_reader.Done()) {
				WireType feature_wire;
				const auto feature_field = feature_reader.ReadTag(feature_wire);
				switch (feature_field) {
				case 1:
					id = feature_reader.ReadVarint();
					has_id = true;
					break;
				case 2: {
					auto tag_reader = feature_reader.ReadSubMessage();
					while (!tag_reader.Done()) {
						tags.push_back(static_cast<uint32_t>(tag_reader.ReadVarint()));
					}
					break;
				}
				case 3:
					geometry_type = static_cast<MVTGeometryType>(feature_reader.ReadVarint());
					break;
				case 4: {
					auto geometry_reader = feature_reader.ReadSubMessage();
					geometry_bytes.assign(geometry_reader.ptr, geometry_reader.end);
					break;
				}
				default:
					feature_reader.SkipField(feature_wire);
					break;
				}
			}

			MVTFeature feature;
			feature.has_id = has_id;
			feature.id = id;
			layer.features.push_back(std::move(feature));
			pending.emplace_back(std::move(tags), make_pair(geometry_type, std::move(geometry_bytes)));
			break;
		}
		default:
			reader.SkipField(type);
			break;
		}
	}

	const TileProjection projection(zoom, tile_x, tile_y, layer.extent);
	for (idx_t i = 0; i < layer.features.size(); i++) {
		auto &feature = layer.features[i];
		auto &tags = pending[i].first;
		auto &geometry_type = pending[i].second.first;
		auto &geometry_bytes = pending[i].second.second;

		// Tags are (key index, value index) pairs into the layer's tables.
		for (idx_t t = 0; t + 1 < tags.size(); t += 2) {
			const auto key_index = tags[t];
			const auto value_index = tags[t + 1];
			if (key_index >= keys.size() || value_index >= values.size()) {
				throw InvalidInputException("Malformed vector tile: attribute index out of range");
			}
			feature.attributes.emplace_back(keys[key_index], values[value_index]);
		}

		if (!geometry_bytes.empty() && geometry_type != MVTGeometryType::UNKNOWN) {
			ProtoReader geometry_reader(geometry_bytes.data(), geometry_bytes.size());
			const auto parts = DecodeGeometryParts(geometry_reader, projection);
			if (!parts.empty()) {
				feature.wkb = BuildWKB(geometry_type, parts);
			}
		}
	}
	return layer;
}

} // namespace

vector<MVTLayer> DecodeMVT(const_data_ptr_t data, idx_t size, int32_t zoom, int64_t tile_x, int64_t tile_y) {
	vector<MVTLayer> layers;
	ProtoReader reader(data, size);
	while (!reader.Done()) {
		WireType type;
		const auto field = reader.ReadTag(type);
		if (field == 3 && type == WireType::LENGTH_DELIMITED) {
			layers.push_back(DecodeLayer(reader.ReadSubMessage(), zoom, tile_x, tile_y));
		} else {
			reader.SkipField(type);
		}
	}
	return layers;
}

} // namespace duckdb
