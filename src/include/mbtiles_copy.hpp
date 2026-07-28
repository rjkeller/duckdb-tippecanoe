//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// mbtiles_copy.hpp
//
// COPY ... TO '<file>.mbtiles' (FORMAT mbtiles, ...)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/copy_function.hpp"

namespace duckdb {

//! Builds the COPY function that renders rows as GeoJSON and pipes them
//! through tippecanoe to produce a tileset.
//!
//! tippecanoe picks the container from the name of the file it is told to
//! write, so the only difference between the two is which file extension the
//! function claims - the writing path is identical.
CopyFunction GetMBTilesCopyFunction();
CopyFunction GetPMTilesCopyFunction();

} // namespace duckdb
