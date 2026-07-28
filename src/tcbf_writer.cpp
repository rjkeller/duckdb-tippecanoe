#include "tcbf_writer.hpp"

#include "geojson_writer.hpp"
#include "wkb_reader.hpp"
#include "tcbf.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"

#include <cmath>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Projection
//===--------------------------------------------------------------------===//

//! Faithful copy of tippecanoe's lonlat2tile (projection.cpp) at zoom 32,
//! clamps and NaN handling included, so coordinates projected here land on
//! exactly the integers tippecanoe would have produced itself.
static void ProjectToWorld(double lon, double lat, int64_t &x, int64_t &y) {
	const int lat_class = std::fpclassify(lat);
	const int lon_class = std::fpclassify(lon);
	bool bad_lon = false;

	if (lat_class == FP_INFINITE || lat_class == FP_NAN) {
		lat = 89.9;
	}
	if (lon_class == FP_INFINITE || lon_class == FP_NAN) {
		lon = 720;
		bad_lon = true;
	}

	if (lat < -89.9) {
		lat = -89.9;
	}
	if (lat > 89.9) {
		lat = 89.9;
	}
	if (lon < -360 && !bad_lon) {
		lon = -360;
	}
	if (lon > 360 && !bad_lon) {
		lon = 360;
	}

	const double lat_rad = lat * M_PI / 180;
	const double n = 4294967296.0; // 1 << 32

	x = static_cast<int64_t>(std::round(n * ((lon + 180) / 360)));
	y = static_cast<int64_t>(std::round(n * (1 - (std::log(std::tan(lat_rad) + 1 / std::cos(lat_rad)) / M_PI)) / 2));
}

//===--------------------------------------------------------------------===//
// Framing
//===--------------------------------------------------------------------===//

static constexpr idx_t TCBF_BLOCK_HEADER_SIZE = 16;

void WriteTCBFMagic(string &out) {
	out.append(reinterpret_cast<const char *>(TCBF_MAGIC), TCBF_MAGIC_LEN);
}

void BeginTCBFBlock(string &out) {
	out.assign(TCBF_BLOCK_HEADER_SIZE, '\0');
}

bool FinishTCBFBlock(string &out, uint64_t feature_count) {
	if (feature_count == 0) {
		return false;
	}
	// Both header fields are little-endian; this code never runs on a
	// big-endian host because DuckDB itself does not support one.
	const uint64_t payload = out.size() - TCBF_BLOCK_HEADER_SIZE;
	memcpy(&out[0], &payload, sizeof(payload));
	memcpy(&out[8], &feature_count, sizeof(feature_count));
	return true;
}

//===--------------------------------------------------------------------===//
// Geometry: WKB -> draws
//===--------------------------------------------------------------------===//

namespace {

//! Walks a WKB blob and emits the TCBF draw stream (delta-encoded projected
//! integers) into `draws`, counting the ops.
struct TCBFGeometryEncoder {
	explicit TCBFGeometryEncoder(WKBReader &reader, string &draws) : reader(reader), draws(draws) {
	}

	WKBReader &reader;
	string &draws;
	uint64_t ndraws = 0;
	int64_t px = 0;
	int64_t py = 0;
	//! TCBF_GEOM_* of the outermost geometry.
	int geom_type = 0;

	void EmitVertex(int op, double lon, double lat) {
		int64_t x, y;
		ProjectToWorld(lon, lat, x, y);
		draws.push_back(static_cast<char>(op));
		tcbf_append_svarint(draws, x - px);
		tcbf_append_svarint(draws, y - py);
		px = x;
		py = y;
		ndraws++;
	}

	void EmitClosePath() {
		draws.push_back(static_cast<char>(TCBF_OP_CLOSEPATH));
		ndraws++;
	}

	idx_t VertexSize(const WKBGeometryHeader &header) const {
		return sizeof(double) * (2 + (header.has_z ? 1 : 0) + (header.has_m ? 1 : 0));
	}

	//! Reads one vertex, dropping Z and M. Returns false for the NaN vertex
	//! that encodes POINT EMPTY.
	bool ReadVertex(const WKBGeometryHeader &header, double &lon, double &lat) {
		lon = reader.ReadDouble();
		lat = reader.ReadDouble();
		if (header.has_z) {
			reader.ReadDouble();
		}
		if (header.has_m) {
			reader.ReadDouble();
		}
		return !(std::isnan(lon) && std::isnan(lat));
	}

	void EncodePointCoordinates(const WKBGeometryHeader &header) {
		double lon, lat;
		if (ReadVertex(header, lon, lat)) {
			EmitVertex(TCBF_OP_MOVETO, lon, lat);
		}
	}

	void EncodeLineCoordinates(const WKBGeometryHeader &header) {
		const auto count = reader.ReadCount(VertexSize(header));
		for (uint32_t i = 0; i < count; i++) {
			double lon, lat;
			ReadVertex(header, lon, lat);
			EmitVertex(i == 0 ? TCBF_OP_MOVETO : TCBF_OP_LINETO, lon, lat);
		}
	}

	//! All rings of one Polygon, followed by the single CLOSEPATH that marks
	//! the end of that Polygon (see tcbf.hpp for why it is per Polygon).
	void EncodePolygonCoordinates(const WKBGeometryHeader &header) {
		const auto rings = reader.ReadCount(sizeof(uint32_t));
		uint64_t vertices = 0;
		for (uint32_t r = 0; r < rings; r++) {
			const auto count = reader.ReadCount(VertexSize(header));
			for (uint32_t i = 0; i < count; i++) {
				double lon, lat;
				ReadVertex(header, lon, lat);
				EmitVertex(i == 0 ? TCBF_OP_MOVETO : TCBF_OP_LINETO, lon, lat);
				vertices++;
			}
		}
		if (vertices > 0) {
			EmitClosePath();
		}
	}

	void EncodeChild(GeometryType expected) {
		const auto header = ReadGeometryHeader(reader);
		if (header.type != expected) {
			throw InvalidInputException("Malformed WKB geometry: multi-geometry contains a mismatched member");
		}
		switch (expected) {
		case GeometryType::POINT:
			EncodePointCoordinates(header);
			break;
		case GeometryType::LINESTRING:
			EncodeLineCoordinates(header);
			break;
		case GeometryType::POLYGON:
			EncodePolygonCoordinates(header);
			break;
		default:
			throw InternalException("Unexpected member type in WKB multi-geometry");
		}
	}

	void EncodeGeometry() {
		const auto header = ReadGeometryHeader(reader);
		switch (header.type) {
		case GeometryType::POINT:
			geom_type = TCBF_GEOM_POINT;
			EncodePointCoordinates(header);
			break;
		case GeometryType::LINESTRING:
			geom_type = TCBF_GEOM_LINE;
			EncodeLineCoordinates(header);
			break;
		case GeometryType::POLYGON:
			geom_type = TCBF_GEOM_POLYGON;
			EncodePolygonCoordinates(header);
			break;
		case GeometryType::MULTIPOINT:
		case GeometryType::MULTILINESTRING:
		case GeometryType::MULTIPOLYGON: {
			GeometryType member;
			if (header.type == GeometryType::MULTIPOINT) {
				geom_type = TCBF_GEOM_POINT;
				member = GeometryType::POINT;
			} else if (header.type == GeometryType::MULTILINESTRING) {
				geom_type = TCBF_GEOM_LINE;
				member = GeometryType::LINESTRING;
			} else {
				geom_type = TCBF_GEOM_POLYGON;
				member = GeometryType::POLYGON;
			}
			const auto count = reader.ReadCount(sizeof(uint8_t) + sizeof(uint32_t));
			for (uint32_t i = 0; i < count; i++) {
				EncodeChild(member);
			}
			break;
		}
		case GeometryType::GEOMETRYCOLLECTION:
			// A TCBF record carries a single geometry type. Collections are
			// rare enough in tiling workloads that falling back to the GeoJSON
			// path (BINARY false) is the supported route.
			throw InvalidInputException(
			    "GeometryCollection cannot be sent over the binary tippecanoe protocol; use BINARY false");
		default:
			throw InvalidInputException("Malformed WKB geometry: unknown geometry type");
		}
	}
};

} // namespace

//===--------------------------------------------------------------------===//
// Properties
//===--------------------------------------------------------------------===//

static void AppendTaggedValue(string &out, int tag, const char *data, idx_t size) {
	out.push_back(static_cast<char>(tag));
	tcbf_append_uvarint(out, size);
	out.append(data, size);
}

static void AppendTaggedValue(string &out, int tag, const string &text) {
	AppendTaggedValue(out, tag, text.data(), text.size());
}

static void AppendIntegerValue(string &out, int64_t value) {
	char buffer[24];
	const auto length = snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
	AppendTaggedValue(out, TCBF_VAL_DOUBLE, buffer, NumericCast<idx_t>(length));
}

static void AppendUnsignedIntegerValue(string &out, uint64_t value) {
	char buffer[24];
	const auto length = snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
	AppendTaggedValue(out, TCBF_VAL_DOUBLE, buffer, NumericCast<idx_t>(length));
}

static void AppendDoubleValue(string &out, double value) {
	if (!std::isfinite(value)) {
		// The GeoJSON path writes these as JSON null; mvt_null says the same.
		AppendTaggedValue(out, TCBF_VAL_NULL, "", 0);
		return;
	}
	char buffer[DOUBLE_BUFFER_SIZE];
	AppendTaggedValue(out, TCBF_VAL_DOUBLE, buffer, FormatDoubleExact(value, buffer));
}

//! Appends the tagged value for row `row` of `source`. The caller has already
//! established that the row is non-NULL.
static void AppendPropertyValue(Vector &source, idx_t row, string &out) {
	auto &type = source.GetType();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		if (FlatVector::GetData<bool>(source)[row]) {
			AppendTaggedValue(out, TCBF_VAL_BOOL, "true", 4);
		} else {
			AppendTaggedValue(out, TCBF_VAL_BOOL, "false", 5);
		}
		return;
	case LogicalTypeId::TINYINT:
		AppendIntegerValue(out, FlatVector::GetData<int8_t>(source)[row]);
		return;
	case LogicalTypeId::SMALLINT:
		AppendIntegerValue(out, FlatVector::GetData<int16_t>(source)[row]);
		return;
	case LogicalTypeId::INTEGER:
		AppendIntegerValue(out, FlatVector::GetData<int32_t>(source)[row]);
		return;
	case LogicalTypeId::BIGINT:
		AppendIntegerValue(out, FlatVector::GetData<int64_t>(source)[row]);
		return;
	case LogicalTypeId::UTINYINT:
		AppendUnsignedIntegerValue(out, FlatVector::GetData<uint8_t>(source)[row]);
		return;
	case LogicalTypeId::USMALLINT:
		AppendUnsignedIntegerValue(out, FlatVector::GetData<uint16_t>(source)[row]);
		return;
	case LogicalTypeId::UINTEGER:
		AppendUnsignedIntegerValue(out, FlatVector::GetData<uint32_t>(source)[row]);
		return;
	case LogicalTypeId::UBIGINT:
		AppendUnsignedIntegerValue(out, FlatVector::GetData<uint64_t>(source)[row]);
		return;
	case LogicalTypeId::FLOAT:
		AppendDoubleValue(out, static_cast<double>(FlatVector::GetData<float>(source)[row]));
		return;
	case LogicalTypeId::DOUBLE:
		AppendDoubleValue(out, FlatVector::GetData<double>(source)[row]);
		return;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		AppendTaggedValue(out, TCBF_VAL_DOUBLE, source.GetValue(row).ToString());
		return;
	case LogicalTypeId::VARCHAR: {
		auto &text = FlatVector::GetData<string_t>(source)[row];
		// JSON-typed text is passed along verbatim too: tippecanoe stores
		// nested values as stringified JSON, which this already is.
		AppendTaggedValue(out, TCBF_VAL_STRING, text.GetData(), text.GetSize());
		return;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP: {
		// Nested values travel as their JSON rendering, matching how the
		// GeoJSON path presents them to tippecanoe.
		string json;
		WriteValueAsJSON(source.GetValue(row), json);
		AppendTaggedValue(out, TCBF_VAL_STRING, json);
		return;
	}
	default: {
		// Dates, timestamps, UUIDs and friends become plain strings, exactly
		// the text the GeoJSON path would have quoted.
		AppendTaggedValue(out, TCBF_VAL_STRING, source.GetValue(row).ToString());
		return;
	}
	}
}

//! The GeoJSON path forwards any JSON value as the feature id and lets
//! tippecanoe sort it out; tippecanoe itself only keeps non-negative integer
//! ids. TCBF encodes exactly that, so only such values produce an id here.
static bool TryGetFeatureId(Vector &source, idx_t row, uint64_t &id) {
	switch (source.GetType().id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT: {
		int64_t value;
		switch (source.GetType().id()) {
		case LogicalTypeId::TINYINT:
			value = FlatVector::GetData<int8_t>(source)[row];
			break;
		case LogicalTypeId::SMALLINT:
			value = FlatVector::GetData<int16_t>(source)[row];
			break;
		case LogicalTypeId::INTEGER:
			value = FlatVector::GetData<int32_t>(source)[row];
			break;
		default:
			value = FlatVector::GetData<int64_t>(source)[row];
			break;
		}
		if (value < 0) {
			return false;
		}
		id = static_cast<uint64_t>(value);
		return true;
	}
	case LogicalTypeId::UTINYINT:
		id = FlatVector::GetData<uint8_t>(source)[row];
		return true;
	case LogicalTypeId::USMALLINT:
		id = FlatVector::GetData<uint16_t>(source)[row];
		return true;
	case LogicalTypeId::UINTEGER:
		id = FlatVector::GetData<uint32_t>(source)[row];
		return true;
	case LogicalTypeId::UBIGINT:
		id = FlatVector::GetData<uint64_t>(source)[row];
		return true;
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// Feature records
//===--------------------------------------------------------------------===//

bool WriteTCBFFeature(DataChunk &chunk, idx_t row, idx_t geometry_wkb, idx_t longitude_index, idx_t latitude_index,
                      idx_t id_index, const vector<idx_t> &property_indexes, const vector<string> &names,
                      TCBFScratch &scratch, string &out) {
	auto &draws = scratch.draws;
	draws.clear();
	int geom_type;
	uint64_t ndraws;

	if (geometry_wkb != DConstants::INVALID_INDEX) {
		auto &source = chunk.data[geometry_wkb];
		if (!FlatVector::Validity(source).RowIsValid(row)) {
			return false;
		}
		auto &blob = FlatVector::GetData<string_t>(source)[row];
		WKBReader reader(const_data_ptr_cast(blob.GetData()), blob.GetSize());
		TCBFGeometryEncoder encoder(reader, draws);
		encoder.EncodeGeometry();
		if (encoder.ndraws == 0) {
			// Empty geometry: the GeoJSON path sends null for tippecanoe to
			// drop; here the feature is simply not sent.
			return false;
		}
		geom_type = encoder.geom_type;
		ndraws = encoder.ndraws;
	} else {
		auto &longitude = chunk.data[longitude_index];
		auto &latitude = chunk.data[latitude_index];
		if (!FlatVector::Validity(longitude).RowIsValid(row) || !FlatVector::Validity(latitude).RowIsValid(row)) {
			return false;
		}
		int64_t x, y;
		ProjectToWorld(FlatVector::GetData<double>(longitude)[row], FlatVector::GetData<double>(latitude)[row], x, y);
		draws.push_back(static_cast<char>(TCBF_OP_MOVETO));
		tcbf_append_svarint(draws, x);
		tcbf_append_svarint(draws, y);
		geom_type = TCBF_GEOM_POINT;
		ndraws = 1;
	}

	auto &record = scratch.record;
	record.clear();
	record.push_back(static_cast<char>(geom_type));

	uint64_t id = 0;
	bool has_id = false;
	if (id_index != DConstants::INVALID_INDEX) {
		auto &source = chunk.data[id_index];
		if (FlatVector::Validity(source).RowIsValid(row)) {
			has_id = TryGetFeatureId(source, row, id);
		}
	}
	record.push_back(has_id ? 1 : 0);
	if (has_id) {
		tcbf_append_uvarint(record, id);
	}

	tcbf_append_uvarint(record, ndraws);
	record.append(draws);

	// NULL properties are omitted entirely, as on the GeoJSON path.
	uint64_t nprops = 0;
	for (auto index : property_indexes) {
		if (FlatVector::Validity(chunk.data[index]).RowIsValid(row)) {
			nprops++;
		}
	}
	tcbf_append_uvarint(record, nprops);
	for (auto index : property_indexes) {
		auto &source = chunk.data[index];
		if (!FlatVector::Validity(source).RowIsValid(row)) {
			continue;
		}
		auto &name = names[index];
		tcbf_append_uvarint(record, name.size());
		record.append(name);
		AppendPropertyValue(source, row, record);
	}

	tcbf_append_uvarint(out, record.size());
	out.append(record);
	return true;
}

} // namespace duckdb
