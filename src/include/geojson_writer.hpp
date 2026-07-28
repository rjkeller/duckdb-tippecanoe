//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// geojson_writer.hpp
//
// Serialization of DuckDB values and WKB geometries into the GeoJSON text that
// is handed to tippecanoe.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Largest string any of the double formatters below can produce, including the
//! terminating null byte.
static constexpr idx_t DOUBLE_BUFFER_SIZE = 64;

//! Coordinates are emitted with this many decimals unless overridden. Seven
//! decimal degrees is roughly a centimetre, an order of magnitude finer than a
//! pixel at tippecanoe's maximum zoom, so the digits we drop cannot survive
//! tiling anyway - and dropping them keeps the pipe to tippecanoe much smaller.
static constexpr int DEFAULT_COORDINATE_PRECISION = 7;

//! Appends `value` as a JSON string literal, including quotes and RFC 8259 escaping.
void WriteJSONString(const char *value, idx_t size, string &out);
void WriteJSONString(const string &value, string &out);

//! Formats a double using the shortest representation that still round-trips.
//! Writes at most DOUBLE_BUFFER_SIZE bytes into `out` and returns the length.
idx_t FormatDoubleExact(double value, char *out);

//! Formats a coordinate with at most `precision` decimals, trailing zeros
//! stripped. A negative precision selects FormatDoubleExact instead.
idx_t FormatCoordinate(double value, int precision, char *out);

//! Appends a value as JSON. Handles nested types recursively; types with no
//! natural JSON counterpart (dates, blobs, intervals, ...) become strings.
void WriteValueAsJSON(const Value &value, string &out);

//! Appends row `row_index` of a *flat* vector as JSON. Equivalent to
//! WriteValueAsJSON(vector.GetValue(row_index)) but avoids materializing a
//! Value for the common scalar types.
void WriteVectorEntryAsJSON(Vector &source, idx_t row_index, string &out);

//! Decodes a WKB blob and appends the equivalent GeoJSON geometry object.
//! Accepts both ISO WKB (dimensionality in the type code) and EWKB (high bits),
//! in either byte order. Throws InvalidInputException on malformed input.
void WriteWKBAsGeoJSON(const_data_ptr_t data, idx_t size, int precision, string &out);

//! Appends a GeoJSON Point built from a raw longitude/latitude pair.
void WritePointAsGeoJSON(double longitude, double latitude, int precision, string &out);

} // namespace duckdb
