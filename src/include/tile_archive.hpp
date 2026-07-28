//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// tile_archive.hpp
//
// A tileset on disk, whether .mbtiles (SQLite) or .pmtiles (single-file
// archive), presented as one stream of tiles.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

class ClientContext;

//! One stored tile, addressed in XYZ convention.
struct TileRecord {
	int32_t zoom;
	int64_t x;
	int64_t y;
	//! Already decompressed, ready to be decoded as MVT.
	string data;
};

//! A tileset being read. Implementations stream tiles in whatever order the
//! underlying format stores them.
class TileArchive {
public:
	virtual ~TileArchive() = default;

	//! Fetches the next tile, returning false once the archive is exhausted.
	//! Tiles outside the requested zoom are skipped by the implementation.
	virtual bool NextTile(TileRecord &record) = 0;

	//! The tileset's JSON metadata, which carries the vector_layers block that
	//! the reader turns into columns. Empty when the tileset has none.
	virtual string GetMetadataJSON() = 0;

	//! The highest zoom the tileset holds, used as the default read level.
	virtual optional_idx GetMaxZoom() = 0;

	//! Restricts the scan to a single zoom. Must be called before NextTile.
	virtual void SetZoomFilter(idx_t zoom) = 0;
};

//! Opens a .mbtiles file.
unique_ptr<TileArchive> OpenMBTilesArchive(ClientContext &context, const string &path);

//! Opens a .pmtiles file.
unique_ptr<TileArchive> OpenPMTilesArchive(ClientContext &context, const string &path);

//! Reads the vector_layers block of a tileset's JSON metadata into a list of
//! attribute columns. Shared by both formats, which both get this from
//! tippecanoe in the same shape.
struct TileAttributeColumn {
	string name;
	LogicalType type;
};
vector<TileAttributeColumn> ParseAttributeColumns(const string &metadata_json);

} // namespace duckdb
