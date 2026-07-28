//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// mbtiles_reader.hpp
//
// SELECT * FROM 'tiles.mbtiles' / 'tiles.pmtiles'
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! One row per feature, with the tileset's own attributes promoted to columns.
TableFunction GetReadMBTilesFunction();
TableFunction GetReadPMTilesFunction();

//! One row per stored tile, geometry left encoded.
TableFunction GetReadMBTilesTilesFunction();
TableFunction GetReadPMTilesTilesFunction();

} // namespace duckdb
