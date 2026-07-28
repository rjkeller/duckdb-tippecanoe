//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// tcbf_writer.hpp
//
// Serialization of DuckDB rows into TCBF, the binary feature stream the
// vendored tippecanoe accepts in place of GeoJSON text. The wire format is
// defined in third_party/tippecanoe/tcbf.hpp; feeding it skips GeoJSON
// generation here and JSON parsing plus coordinate re-projection there.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Scratch buffers reused across rows so record assembly does not allocate.
struct TCBFScratch {
	string record;
	string draws;
};

//! Appends the 6-byte TCBF stream magic.
void WriteTCBFMagic(string &out);

//! Reserves space for a block header at the start of `out`. Blocks are framed
//! as [u64le payload bytes][u64le feature count][payload].
void BeginTCBFBlock(string &out);

//! Patches the block header once the payload is complete. Returns false if the
//! block holds no features (nothing needs writing).
bool FinishTCBFBlock(string &out, uint64_t feature_count);

//! Appends one feature record built from `row` of `chunk`.
//!
//! `geometry_wkb` selects a GEOMETRY column to walk; when it is invalid,
//! `longitude_index`/`latitude_index` supply a Point instead. Returns false -
//! writing nothing - for rows whose geometry is NULL or empty, mirroring the
//! GeoJSON path, where such features are sent as null geometry for tippecanoe
//! to drop.
bool WriteTCBFFeature(DataChunk &chunk, idx_t row, idx_t geometry_wkb, idx_t longitude_index, idx_t latitude_index,
                      idx_t id_index, const vector<idx_t> &property_indexes, const vector<string> &names,
                      TCBFScratch &scratch, string &out);

} // namespace duckdb
