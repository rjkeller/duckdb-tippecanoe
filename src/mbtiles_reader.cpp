#include "mbtiles_reader.hpp"

#include "mvt_decoder.hpp"
#include "tile_archive.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/main/client_context.hpp"
#include "yyjson.hpp"

#include "sqlite3.h"

using namespace duckdb_yyjson; // NOLINT: matches how DuckDB's own JSON code reads

namespace duckdb {

//===--------------------------------------------------------------------===//
// Schema, taken from the tileset's own metadata
//===--------------------------------------------------------------------===//

vector<TileAttributeColumn> ParseAttributeColumns(const string &metadata_json) {
	vector<TileAttributeColumn> columns;
	if (metadata_json.empty()) {
		return columns;
	}
	auto *doc = yyjson_read(metadata_json.c_str(), metadata_json.size(), 0);
	if (!doc) {
		return columns;
	}
	auto *root = yyjson_doc_get_root(doc);
	// .mbtiles nests this under a "json" metadata row that has already been
	// unwrapped by the caller; .pmtiles stores the same object at the top level.
	auto *layers = root ? yyjson_obj_get(root, "vector_layers") : nullptr;

	// A name may appear in several layers; keep the first type seen and fall
	// back to VARCHAR when layers disagree about it.
	case_insensitive_map_t<idx_t> seen;
	if (layers && yyjson_is_arr(layers)) {
		size_t layer_index, layer_max;
		yyjson_val *layer;
		yyjson_arr_foreach(layers, layer_index, layer_max, layer) {
			auto *fields = yyjson_obj_get(layer, "fields");
			if (!fields || !yyjson_is_obj(fields)) {
				continue;
			}
			size_t field_index, field_max;
			yyjson_val *key, *value;
			yyjson_obj_foreach(fields, field_index, field_max, key, value) {
				const auto *name = yyjson_get_str(key);
				const auto *kind = yyjson_get_str(value);
				if (!name) {
					continue;
				}
				LogicalType type = LogicalType::VARCHAR;
				if (kind) {
					const string kind_text = kind;
					if (StringUtil::CIEquals(kind_text, "Number")) {
						type = LogicalType::DOUBLE;
					} else if (StringUtil::CIEquals(kind_text, "Boolean")) {
						type = LogicalType::BOOLEAN;
					}
				}
				auto entry = seen.find(name);
				if (entry == seen.end()) {
					seen[name] = columns.size();
					columns.push_back(TileAttributeColumn {name, type});
				} else if (columns[entry->second].type != type) {
					columns[entry->second].type = LogicalType::VARCHAR;
				}
			}
		}
	}
	yyjson_doc_free(doc);
	return columns;
}

//===--------------------------------------------------------------------===//
// .mbtiles: a SQLite database
//===--------------------------------------------------------------------===//

namespace {

class MBTilesArchive : public TileArchive {
public:
	MBTilesArchive(ClientContext &, const string &path) : path(path) {
		// Read-only, and deliberately not immutable: the tileset may still be
		// being written by another process.
		const auto rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
		if (rc != SQLITE_OK) {
			const string message = db ? sqlite3_errmsg(db) : "unable to open database";
			if (db) {
				sqlite3_close(db);
				db = nullptr;
			}
			throw IOException("Failed to open \"%s\" as an mbtiles (SQLite) file: %s", path, message);
		}
	}

	~MBTilesArchive() override {
		if (statement) {
			sqlite3_finalize(statement);
		}
		if (db) {
			sqlite3_close(db);
		}
	}

	void SetZoomFilter(idx_t zoom) override {
		zoom_filter = zoom;
	}

	bool NextTile(TileRecord &record) override {
		// sqlite3_step() on a statement that already returned SQLITE_DONE
		// silently resets it and runs the query again from the start, so
		// exhaustion has to be latched or the scan never ends.
		if (finished) {
			return false;
		}
		if (!statement) {
			Prepare();
		}
		while (true) {
			const auto rc = sqlite3_step(statement);
			if (rc == SQLITE_DONE) {
				finished = true;
				return false;
			}
			if (rc != SQLITE_ROW) {
				throw IOException("Failed while reading tiles from \"%s\": %s", path, sqlite3_errmsg(db));
			}
			const auto zoom = sqlite3_column_int(statement, 0);
			const auto column = sqlite3_column_int64(statement, 1);
			const auto row = sqlite3_column_int64(statement, 2);
			const auto *blob = static_cast<const_data_ptr_t>(sqlite3_column_blob(statement, 3));
			const auto blob_size = NumericCast<idx_t>(sqlite3_column_bytes(statement, 3));
			if (!blob || blob_size == 0) {
				continue;
			}
			auto tile = MaybeGunzipTile(blob, blob_size);
			if (tile.empty()) {
				continue;
			}
			record.zoom = zoom;
			record.x = column;
			// .mbtiles numbers rows from the bottom (TMS); everything else
			// addresses tiles from the top.
			record.y = (int64_t(1) << zoom) - 1 - row;
			record.data = std::move(tile);
			return true;
		}
	}

	string GetMetadataJSON() override {
		string result;
		QueryScalar("SELECT value FROM metadata WHERE name = 'json'", result);
		return result;
	}

	optional_idx GetMaxZoom() override {
		string text;
		if (QueryScalar("SELECT value FROM metadata WHERE name = 'maxzoom'", text) ||
		    QueryScalar("SELECT max(zoom_level) FROM tiles", text)) {
			try {
				return NumericCast<idx_t>(std::stoll(text));
			} catch (const std::exception &) {
				return optional_idx();
			}
		}
		return optional_idx();
	}

private:
	void Prepare() {
		string sql = "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles";
		if (zoom_filter.IsValid()) {
			sql += StringUtil::Format(" WHERE zoom_level = %lld", static_cast<long long>(zoom_filter.GetIndex()));
		}
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
			throw IOException("Failed to query \"%s\": %s", path, sqlite3_errmsg(db));
		}
	}

	bool QueryScalar(const string &sql, string &result) {
		sqlite3_stmt *stmt = nullptr;
		if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
			return false;
		}
		bool found = false;
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
			if (text) {
				result = text;
				found = true;
			}
		}
		sqlite3_finalize(stmt);
		return found;
	}

	string path;
	sqlite3 *db = nullptr;
	sqlite3_stmt *statement = nullptr;
	optional_idx zoom_filter;
	bool finished = false;
};

} // namespace

unique_ptr<TileArchive> OpenMBTilesArchive(ClientContext &context, const string &path) {
	return make_uniq<MBTilesArchive>(context, path);
}

//===--------------------------------------------------------------------===//
// The table function, shared by both formats
//===--------------------------------------------------------------------===//

namespace {

//! Which archive a bound read is going to open.
enum class TileArchiveKind : uint8_t { MBTILES, PMTILES };

unique_ptr<TileArchive> OpenArchive(ClientContext &context, TileArchiveKind kind, const string &path) {
	return kind == TileArchiveKind::MBTILES ? OpenMBTilesArchive(context, path) : OpenPMTilesArchive(context, path);
}

struct TileReadBindData : public TableFunctionData {
	TileArchiveKind kind;
	string path;
	vector<TileAttributeColumn> attributes;
	//! Zoom to read. Reading every level would repeat each feature once per
	//! level it survived to, so this always resolves to a single one.
	optional_idx zoom;
	string layer_filter;
	//! Columns that come before the attribute columns.
	static constexpr idx_t FIXED_COLUMNS = 6;
};

struct TileReadGlobalState : public GlobalTableFunctionState {
	unique_ptr<TileArchive> archive;
	//! Features decoded from the tile currently being drained.
	vector<pair<string, MVTFeature>> pending;
	idx_t pending_offset = 0;
	bool exhausted = false;
	int32_t current_zoom = 0;
	int64_t current_x = 0;
	int64_t current_y = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

template <TileArchiveKind KIND>
unique_ptr<FunctionData> TileReadBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<TileReadBindData>();
	result->kind = KIND;
	result->path = input.inputs[0].GetValue<string>();

	for (auto &option : input.named_parameters) {
		const auto name = StringUtil::Lower(option.first);
		if (name == "zoom") {
			const auto zoom = option.second.CastAs(context, LogicalType::BIGINT).GetValue<int64_t>();
			if (zoom < 0 || zoom > 32) {
				throw BinderException("ZOOM must be between 0 and 32");
			}
			result->zoom = NumericCast<idx_t>(zoom);
		} else if (name == "layer") {
			result->layer_filter = option.second.CastAs(context, LogicalType::VARCHAR).GetValue<string>();
		}
	}

	auto archive = OpenArchive(context, KIND, result->path);
	result->attributes = ParseAttributeColumns(archive->GetMetadataJSON());

	// Default to the tileset's maximum zoom, the only level where every
	// surviving feature is present.
	if (!result->zoom.IsValid()) {
		result->zoom = archive->GetMaxZoom();
		if (!result->zoom.IsValid()) {
			throw IOException("Could not determine a zoom level to read from \"%s\"", result->path);
		}
	}

	names = {"geom", "layer", "zoom", "tile_x", "tile_y", "feature_id"};
	return_types = {LogicalType::GEOMETRY(), LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::INTEGER,    LogicalType::INTEGER, LogicalType::BIGINT};
	for (auto &attribute : result->attributes) {
		names.push_back(attribute.name);
		return_types.push_back(attribute.type);
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> TileReadInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TileReadBindData>();
	auto result = make_uniq<TileReadGlobalState>();
	result->archive = OpenArchive(context, bind_data.kind, bind_data.path);
	result->archive->SetZoomFilter(bind_data.zoom.GetIndex());
	return std::move(result);
}

//! Pulls tiles until one yields at least one feature.
bool LoadNextTile(const TileReadBindData &bind_data, TileReadGlobalState &state) {
	TileRecord record;
	while (state.archive->NextTile(record)) {
		auto layers = DecodeMVT(const_data_ptr_cast(record.data.c_str()), record.data.size(), record.zoom, record.x,
		                        record.y);
		state.pending.clear();
		state.pending_offset = 0;
		for (auto &layer : layers) {
			if (!bind_data.layer_filter.empty() && !StringUtil::CIEquals(layer.name, bind_data.layer_filter)) {
				continue;
			}
			for (auto &feature : layer.features) {
				if (feature.wkb.empty()) {
					continue;
				}
				state.pending.emplace_back(layer.name, std::move(feature));
			}
		}
		state.current_zoom = record.zoom;
		state.current_x = record.x;
		state.current_y = record.y;
		if (!state.pending.empty()) {
			return true;
		}
	}
	state.exhausted = true;
	return false;
}

void TileReadFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<TileReadBindData>();
	auto &state = input.global_state->Cast<TileReadGlobalState>();

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (state.pending_offset >= state.pending.size()) {
			if (state.exhausted || !LoadNextTile(bind_data, state)) {
				break;
			}
		}
		auto &entry = state.pending[state.pending_offset++];
		auto &layer_name = entry.first;
		auto &feature = entry.second;

		// GEOMETRY is WKB internally, so the decoded bytes go straight in.
		FlatVector::GetData<string_t>(output.data[0])[count] =
		    StringVector::AddStringOrBlob(output.data[0], feature.wkb.c_str(), feature.wkb.size());
		FlatVector::GetData<string_t>(output.data[1])[count] = StringVector::AddString(output.data[1], layer_name);
		FlatVector::GetData<int32_t>(output.data[2])[count] = state.current_zoom;
		FlatVector::GetData<int32_t>(output.data[3])[count] = NumericCast<int32_t>(state.current_x);
		FlatVector::GetData<int32_t>(output.data[4])[count] = NumericCast<int32_t>(state.current_y);
		if (feature.has_id) {
			FlatVector::GetData<int64_t>(output.data[5])[count] = NumericCast<int64_t>(feature.id);
		} else {
			FlatVector::Validity(output.data[5]).SetInvalid(count);
		}

		for (idx_t a = 0; a < bind_data.attributes.size(); a++) {
			auto &column = output.data[TileReadBindData::FIXED_COLUMNS + a];
			const auto &wanted = bind_data.attributes[a].name;
			bool found = false;
			for (auto &attribute : feature.attributes) {
				if (StringUtil::CIEquals(attribute.first, wanted)) {
					if (attribute.second.IsNull()) {
						break;
					}
					column.SetValue(count, attribute.second.DefaultCastAs(column.GetType()));
					found = true;
					break;
				}
			}
			if (!found) {
				FlatVector::Validity(column).SetInvalid(count);
			}
		}
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Raw tiles
//===--------------------------------------------------------------------===//

struct RawTilesBindData : public TableFunctionData {
	TileArchiveKind kind;
	string path;
};

struct RawTilesGlobalState : public GlobalTableFunctionState {
	unique_ptr<TileArchive> archive;
	//! Latched so that an archive which fails to report exhaustion stably can
	//! never turn this scan into an endless one.
	bool exhausted = false;
	idx_t MaxThreads() const override {
		return 1;
	}
};

template <TileArchiveKind KIND>
unique_ptr<FunctionData> RawTilesBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<RawTilesBindData>();
	result->kind = KIND;
	result->path = input.inputs[0].GetValue<string>();
	OpenArchive(context, KIND, result->path); // fail at bind time rather than mid-scan
	names = {"zoom", "tile_x", "tile_y", "tile_data"};
	return_types = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::BLOB};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> RawTilesInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RawTilesBindData>();
	auto result = make_uniq<RawTilesGlobalState>();
	result->archive = OpenArchive(context, bind_data.kind, bind_data.path);
	return std::move(result);
}

void RawTilesFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<RawTilesGlobalState>();
	idx_t count = 0;
	TileRecord record;
	while (count < STANDARD_VECTOR_SIZE && !state.exhausted) {
		if (!state.archive->NextTile(record)) {
			state.exhausted = true;
			break;
		}
		FlatVector::GetData<int32_t>(output.data[0])[count] = record.zoom;
		FlatVector::GetData<int32_t>(output.data[1])[count] = NumericCast<int32_t>(record.x);
		FlatVector::GetData<int32_t>(output.data[2])[count] = NumericCast<int32_t>(record.y);
		FlatVector::GetData<string_t>(output.data[3])[count] =
		    StringVector::AddStringOrBlob(output.data[3], record.data.c_str(), record.data.size());
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

TableFunction GetReadMBTilesFunction() {
	TableFunction function("read_mbtiles", {LogicalType::VARCHAR}, TileReadFunction,
	                       TileReadBind<TileArchiveKind::MBTILES>, TileReadInitGlobal);
	function.named_parameters["zoom"] = LogicalType::BIGINT;
	function.named_parameters["layer"] = LogicalType::VARCHAR;
	return function;
}

TableFunction GetReadPMTilesFunction() {
	TableFunction function("read_pmtiles", {LogicalType::VARCHAR}, TileReadFunction,
	                       TileReadBind<TileArchiveKind::PMTILES>, TileReadInitGlobal);
	function.named_parameters["zoom"] = LogicalType::BIGINT;
	function.named_parameters["layer"] = LogicalType::VARCHAR;
	return function;
}

TableFunction GetReadMBTilesTilesFunction() {
	return TableFunction("read_mbtiles_tiles", {LogicalType::VARCHAR}, RawTilesFunction,
	                     RawTilesBind<TileArchiveKind::MBTILES>, RawTilesInitGlobal);
}

TableFunction GetReadPMTilesTilesFunction() {
	return TableFunction("read_pmtiles_tiles", {LogicalType::VARCHAR}, RawTilesFunction,
	                     RawTilesBind<TileArchiveKind::PMTILES>, RawTilesInitGlobal);
}

} // namespace duckdb
