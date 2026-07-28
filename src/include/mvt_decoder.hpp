//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// mvt_decoder.hpp
//
// Decoding of Mapbox Vector Tiles back into geometries and attributes.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

//! One decoded feature: WKB in WGS84, plus whatever attributes it carried.
struct MVTFeature {
	//! Standard little-endian WKB, ready to be handed to a GEOMETRY vector.
	string wkb;
	//! Present only when the tile actually carried an id for this feature.
	bool has_id = false;
	uint64_t id = 0;
	//! Attribute name -> value, already converted to a DuckDB Value.
	vector<pair<string, Value>> attributes;
};

//! One decoded layer of a tile.
struct MVTLayer {
	string name;
	uint32_t extent = 4096;
	vector<MVTFeature> features;
};

//! Decodes a whole vector tile.
//!
//! `zoom`, `tile_x` and `tile_y` are the tile's address in XYZ convention -
//! note that .mbtiles stores rows in TMS order, so callers must flip the row
//! before calling. They are needed because MVT coordinates are tile-local and
//! only become longitude/latitude once the tile's position is known.
//!
//! Throws InvalidInputException if the buffer is not a well-formed tile.
vector<MVTLayer> DecodeMVT(const_data_ptr_t data, idx_t size, int32_t zoom, int64_t tile_x, int64_t tile_y);

//! Decompresses a tile if it is gzip-wrapped, otherwise returns it unchanged.
//! .mbtiles tiles are conventionally gzipped but the spec permits raw ones.
string MaybeGunzipTile(const_data_ptr_t data, idx_t size);

} // namespace duckdb
