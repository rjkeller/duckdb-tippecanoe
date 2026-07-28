#include "geojson_writer.hpp"
#include "wkb_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Scalar formatting
//===--------------------------------------------------------------------===//

void WriteJSONString(const char *value, idx_t size, string &out) {
	out += '"';
	// Copy in runs so that the common case - no character needs escaping - is a
	// single append rather than a byte-at-a-time loop.
	idx_t run_start = 0;
	for (idx_t i = 0; i < size; i++) {
		const auto c = static_cast<unsigned char>(value[i]);
		const char *escape;
		char unicode_escape[7];
		switch (c) {
		case '"':
			escape = "\\\"";
			break;
		case '\\':
			escape = "\\\\";
			break;
		case '\b':
			escape = "\\b";
			break;
		case '\f':
			escape = "\\f";
			break;
		case '\n':
			escape = "\\n";
			break;
		case '\r':
			escape = "\\r";
			break;
		case '\t':
			escape = "\\t";
			break;
		default:
			if (c >= 0x20) {
				continue;
			}
			snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", c);
			escape = unicode_escape;
			break;
		}
		out.append(value + run_start, i - run_start);
		out += escape;
		run_start = i + 1;
	}
	out.append(value + run_start, size - run_start);
	out += '"';
}

void WriteJSONString(const string &value, string &out) {
	WriteJSONString(value.c_str(), value.size(), out);
}

idx_t FormatDoubleExact(double value, char *out) {
	if (!std::isfinite(value)) {
		// JSON has no way to spell NaN or infinity.
		memcpy(out, "null", 5);
		return 4;
	}
	// 15 significant digits round-trip for the overwhelming majority of doubles
	// and read far better than the 17 that are needed in the worst case.
	auto length = snprintf(out, DOUBLE_BUFFER_SIZE, "%.15g", value);
	if (length > 0 && std::strtod(out, nullptr) != value) {
		length = snprintf(out, DOUBLE_BUFFER_SIZE, "%.17g", value);
	}
	if (length <= 0) {
		memcpy(out, "null", 5);
		return 4;
	}
	return NumericCast<idx_t>(length);
}

idx_t FormatCoordinate(double value, int precision, char *out) {
	if (precision < 0) {
		return FormatDoubleExact(value, out);
	}
	if (!std::isfinite(value)) {
		memcpy(out, "null", 5);
		return 4;
	}
	const auto written = snprintf(out, DOUBLE_BUFFER_SIZE, "%.*f", precision, value);
	if (written <= 0) {
		memcpy(out, "0", 2);
		return 1;
	}
	auto length = MinValue<idx_t>(NumericCast<idx_t>(written), DOUBLE_BUFFER_SIZE - 1);
	// "%f" never uses an exponent, so a '.' means there are decimals to trim.
	if (memchr(out, '.', length)) {
		while (length > 0 && out[length - 1] == '0') {
			length--;
		}
		if (length > 0 && out[length - 1] == '.') {
			length--;
		}
	}
	// Rounding a tiny negative number down to zero must not leave "-0".
	if (length == 2 && out[0] == '-' && out[1] == '0') {
		out[0] = '0';
		length = 1;
	}
	out[length] = '\0';
	return length;
}

static void AppendInteger(int64_t value, string &out) {
	char buffer[24];
	const auto length = snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
	if (length > 0) {
		out.append(buffer, NumericCast<idx_t>(length));
	}
}

static void AppendUnsignedInteger(uint64_t value, string &out) {
	char buffer[24];
	const auto length = snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
	if (length > 0) {
		out.append(buffer, NumericCast<idx_t>(length));
	}
}

static void AppendDouble(double value, string &out) {
	char buffer[DOUBLE_BUFFER_SIZE];
	out.append(buffer, FormatDoubleExact(value, buffer));
}

//! JSON-typed values are already JSON, so they are spliced in verbatim rather
//! than being quoted and escaped as a string.
static bool IsJSONType(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR && type.HasAlias() && type.GetAlias() == "JSON";
}

//===--------------------------------------------------------------------===//
// Value -> JSON
//===--------------------------------------------------------------------===//

void WriteValueAsJSON(const Value &value, string &out) {
	if (value.IsNull()) {
		out += "null";
		return;
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		out += BooleanValue::Get(value) ? "true" : "false";
		return;
	case LogicalTypeId::TINYINT:
		AppendInteger(TinyIntValue::Get(value), out);
		return;
	case LogicalTypeId::SMALLINT:
		AppendInteger(SmallIntValue::Get(value), out);
		return;
	case LogicalTypeId::INTEGER:
		AppendInteger(IntegerValue::Get(value), out);
		return;
	case LogicalTypeId::BIGINT:
		AppendInteger(BigIntValue::Get(value), out);
		return;
	case LogicalTypeId::UTINYINT:
		AppendUnsignedInteger(UTinyIntValue::Get(value), out);
		return;
	case LogicalTypeId::USMALLINT:
		AppendUnsignedInteger(USmallIntValue::Get(value), out);
		return;
	case LogicalTypeId::UINTEGER:
		AppendUnsignedInteger(UIntegerValue::Get(value), out);
		return;
	case LogicalTypeId::UBIGINT:
		AppendUnsignedInteger(UBigIntValue::Get(value), out);
		return;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		// The default string rendering of these is already a valid JSON number.
		out += value.ToString();
		return;
	case LogicalTypeId::FLOAT:
		AppendDouble(static_cast<double>(FloatValue::Get(value)), out);
		return;
	case LogicalTypeId::DOUBLE:
		AppendDouble(DoubleValue::Get(value), out);
		return;
	case LogicalTypeId::VARCHAR: {
		auto &text = StringValue::Get(value);
		if (IsJSONType(type)) {
			out += text;
		} else {
			WriteJSONString(text, out);
		}
		return;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto &children = type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value)
		                                                  : ArrayValue::GetChildren(value);
		out += '[';
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			WriteValueAsJSON(children[i], out);
		}
		out += ']';
		return;
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		auto &child_types = StructType::GetChildTypes(type);
		out += '{';
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			WriteJSONString(child_types[i].first, out);
			out += ':';
			WriteValueAsJSON(children[i], out);
		}
		out += '}';
		return;
	}
	case LogicalTypeId::MAP: {
		// Rendered as a JSON object; non-string keys are stringified, since JSON
		// object keys can only be strings.
		auto &entries = ListValue::GetChildren(value);
		out += '{';
		for (idx_t i = 0; i < entries.size(); i++) {
			auto &entry = StructValue::GetChildren(entries[i]);
			if (i > 0) {
				out += ',';
			}
			WriteJSONString(entry[0].ToString(), out);
			out += ':';
			WriteValueAsJSON(entry[1], out);
		}
		out += '}';
		return;
	}
	default: {
		// Dates, timestamps, blobs, intervals, enums, UUIDs and friends have no
		// JSON counterpart, so they travel as strings.
		const auto text = value.ToString();
		WriteJSONString(text, out);
		return;
	}
	}
}

void WriteVectorEntryAsJSON(Vector &source, idx_t row_index, string &out) {
	D_ASSERT(source.GetVectorType() == VectorType::FLAT_VECTOR);
	if (!FlatVector::Validity(source).RowIsValid(row_index)) {
		out += "null";
		return;
	}
	auto &type = source.GetType();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		out += FlatVector::GetData<bool>(source)[row_index] ? "true" : "false";
		return;
	case LogicalTypeId::TINYINT:
		AppendInteger(FlatVector::GetData<int8_t>(source)[row_index], out);
		return;
	case LogicalTypeId::SMALLINT:
		AppendInteger(FlatVector::GetData<int16_t>(source)[row_index], out);
		return;
	case LogicalTypeId::INTEGER:
		AppendInteger(FlatVector::GetData<int32_t>(source)[row_index], out);
		return;
	case LogicalTypeId::BIGINT:
		AppendInteger(FlatVector::GetData<int64_t>(source)[row_index], out);
		return;
	case LogicalTypeId::UTINYINT:
		AppendUnsignedInteger(FlatVector::GetData<uint8_t>(source)[row_index], out);
		return;
	case LogicalTypeId::USMALLINT:
		AppendUnsignedInteger(FlatVector::GetData<uint16_t>(source)[row_index], out);
		return;
	case LogicalTypeId::UINTEGER:
		AppendUnsignedInteger(FlatVector::GetData<uint32_t>(source)[row_index], out);
		return;
	case LogicalTypeId::UBIGINT:
		AppendUnsignedInteger(FlatVector::GetData<uint64_t>(source)[row_index], out);
		return;
	case LogicalTypeId::FLOAT:
		AppendDouble(static_cast<double>(FlatVector::GetData<float>(source)[row_index]), out);
		return;
	case LogicalTypeId::DOUBLE:
		AppendDouble(FlatVector::GetData<double>(source)[row_index], out);
		return;
	case LogicalTypeId::VARCHAR: {
		auto &text = FlatVector::GetData<string_t>(source)[row_index];
		if (IsJSONType(type)) {
			out.append(text.GetData(), text.GetSize());
		} else {
			WriteJSONString(text.GetData(), text.GetSize(), out);
		}
		return;
	}
	default:
		// Everything else goes through Value, which knows how to render it.
		WriteValueAsJSON(source.GetValue(row_index), out);
		return;
	}
}

//===--------------------------------------------------------------------===//
// WKB -> GeoJSON
//===--------------------------------------------------------------------===//

namespace {

struct GeoJSONEncoder {
	GeoJSONEncoder(WKBReader &reader, int precision, string &out) : reader(reader), precision(precision), out(out) {
	}

	WKBReader &reader;
	int precision;
	string &out;
	//! Whether the outermost geometry turned out to hold no coordinates at all.
	bool top_level_empty = false;

	void AppendNumber(double value) {
		char buffer[DOUBLE_BUFFER_SIZE];
		out.append(buffer, FormatCoordinate(value, precision, buffer));
	}

	//! Reads one vertex and appends it as [x,y] or [x,y,z]. GeoJSON has no M
	//! dimension, so any M ordinate is read and discarded.
	void WriteVertex(const WKBGeometryHeader &header) {
		const auto x = reader.ReadDouble();
		const auto y = reader.ReadDouble();
		const auto z = header.has_z ? reader.ReadDouble() : 0.0;
		if (header.has_m) {
			reader.ReadDouble();
		}
		out += '[';
		AppendNumber(x);
		out += ',';
		AppendNumber(y);
		if (header.has_z) {
			out += ',';
			AppendNumber(z);
		}
		out += ']';
	}

	idx_t VertexSize(const WKBGeometryHeader &header) const {
		return sizeof(double) * (2 + (header.has_z ? 1 : 0) + (header.has_m ? 1 : 0));
	}

	uint32_t WriteVertexArray(const WKBGeometryHeader &header) {
		const auto count = reader.ReadCount(VertexSize(header));
		out += '[';
		for (uint32_t i = 0; i < count; i++) {
			if (i > 0) {
				out += ',';
			}
			WriteVertex(header);
		}
		out += ']';
		return count;
	}

	uint32_t WriteRingArray(const WKBGeometryHeader &header) {
		// Each ring costs at least a 4-byte count.
		const auto count = reader.ReadCount(sizeof(uint32_t));
		out += '[';
		for (uint32_t i = 0; i < count; i++) {
			if (i > 0) {
				out += ',';
			}
			WriteVertexArray(header);
		}
		out += ']';
		return count;
	}

	//! Writes the "coordinates" of a nested geometry, verifying it has the type
	//! its parent multi-geometry promises.
	void WriteChildCoordinates(GeometryType expected) {
		const auto header = ReadGeometryHeader(reader);
		if (header.type != expected) {
			throw InvalidInputException("Malformed WKB geometry: multi-geometry contains a mismatched member");
		}
		switch (expected) {
		case GeometryType::POINT:
			WritePointCoordinates(header);
			break;
		case GeometryType::LINESTRING:
			WriteVertexArray(header);
			break;
		case GeometryType::POLYGON:
			WriteRingArray(header);
			break;
		default:
			throw InternalException("Unexpected member type in WKB multi-geometry");
		}
	}

	//! An empty point is encoded as a vertex whose ordinates are all NaN, and
	//! GeoJSON spells that as an empty coordinate array. Returns whether the
	//! point was empty.
	bool WritePointCoordinates(const WKBGeometryHeader &header) {
		const auto x = reader.ReadDouble();
		const auto y = reader.ReadDouble();
		const auto z = header.has_z ? reader.ReadDouble() : 0.0;
		if (header.has_m) {
			reader.ReadDouble();
		}
		if (std::isnan(x) && std::isnan(y)) {
			out += "[]";
			return true;
		}
		out += '[';
		AppendNumber(x);
		out += ',';
		AppendNumber(y);
		if (header.has_z) {
			out += ',';
			AppendNumber(z);
		}
		out += ']';
		return false;
	}

	uint32_t WriteMulti(GeometryType member_type) {
		// Every member is at least a byte order marker plus a type code.
		const auto count = reader.ReadCount(sizeof(uint8_t) + sizeof(uint32_t));
		out += '[';
		for (uint32_t i = 0; i < count; i++) {
			if (i > 0) {
				out += ',';
			}
			WriteChildCoordinates(member_type);
		}
		out += ']';
		return count;
	}

	void WriteGeometry(idx_t depth) {
		if (depth > Geometry::MAX_RECURSION_DEPTH) {
			throw InvalidInputException("WKB geometry nests deeper than the supported limit of %llu levels",
			                            static_cast<unsigned long long>(Geometry::MAX_RECURSION_DEPTH));
		}
		const auto header = ReadGeometryHeader(reader);
		bool is_empty;
		switch (header.type) {
		case GeometryType::POINT:
			out += R"({"type":"Point","coordinates":)";
			is_empty = WritePointCoordinates(header);
			break;
		case GeometryType::LINESTRING:
			out += R"({"type":"LineString","coordinates":)";
			is_empty = WriteVertexArray(header) == 0;
			break;
		case GeometryType::POLYGON:
			out += R"({"type":"Polygon","coordinates":)";
			is_empty = WriteRingArray(header) == 0;
			break;
		case GeometryType::MULTIPOINT:
			out += R"({"type":"MultiPoint","coordinates":)";
			is_empty = WriteMulti(GeometryType::POINT) == 0;
			break;
		case GeometryType::MULTILINESTRING:
			out += R"({"type":"MultiLineString","coordinates":)";
			is_empty = WriteMulti(GeometryType::LINESTRING) == 0;
			break;
		case GeometryType::MULTIPOLYGON:
			out += R"({"type":"MultiPolygon","coordinates":)";
			is_empty = WriteMulti(GeometryType::POLYGON) == 0;
			break;
		case GeometryType::GEOMETRYCOLLECTION: {
			out += R"({"type":"GeometryCollection","geometries":[)";
			const auto count = reader.ReadCount(sizeof(uint8_t) + sizeof(uint32_t));
			for (uint32_t i = 0; i < count; i++) {
				if (i > 0) {
					out += ',';
				}
				WriteGeometry(depth + 1);
			}
			out += ']';
			is_empty = count == 0;
			break;
		}
		default:
			throw InvalidInputException("Malformed WKB geometry: unknown geometry type");
		}
		out += '}';
		if (depth == 0) {
			top_level_empty = is_empty;
		}
	}
};

} // namespace

void WriteWKBAsGeoJSON(const_data_ptr_t data, idx_t size, int precision, string &out) {
	WKBReader reader(data, size);
	GeoJSONEncoder encoder(reader, precision, out);
	const auto start = out.size();
	encoder.WriteGeometry(0);
	if (encoder.top_level_empty) {
		// An empty geometry has nothing to put in a tile, and tippecanoe rejects
		// some of the ways GeoJSON spells one - an empty Point in particular is
		// a hard error that would fail the whole tileset. A null geometry says
		// the same thing in a form it always accepts.
		out.resize(start);
		out += "null";
	}
}

void WritePointAsGeoJSON(double longitude, double latitude, int precision, string &out) {
	char buffer[DOUBLE_BUFFER_SIZE];
	out += R"({"type":"Point","coordinates":[)";
	out.append(buffer, FormatCoordinate(longitude, precision, buffer));
	out += ',';
	out.append(buffer, FormatCoordinate(latitude, precision, buffer));
	out += "]}";
}

} // namespace duckdb
