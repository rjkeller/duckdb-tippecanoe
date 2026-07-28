#include "mbtiles_copy.hpp"

#include "geojson_writer.hpp"
#include "tcbf_writer.hpp"
#include "tippecanoe_process.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

//! Features accumulate in a per-thread buffer and are handed to tippecanoe in
//! chunks of roughly this size, to keep the number of write() calls down
//! without letting the buffer grow unbounded.
static constexpr idx_t FLUSH_THRESHOLD_BYTES = 1 << 20;

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

//! Where the geometry of each feature comes from.
enum class GeometryEncoding : uint8_t {
	//! A GEOMETRY column, whose in-flight representation is WKB.
	WKB,
	//! A VARCHAR column already holding a GeoJSON geometry object.
	GEOJSON_TEXT,
	//! A pair of numeric columns forming a Point.
	LONGITUDE_LATITUDE
};

struct GeometryBinding {
	GeometryEncoding encoding = GeometryEncoding::WKB;
	idx_t geometry_index = DConstants::INVALID_INDEX;
	idx_t longitude_index = DConstants::INVALID_INDEX;
	idx_t latitude_index = DConstants::INVALID_INDEX;
};

struct MBTilesWriteBindData : public FunctionData {
	//! Path of the tippecanoe binary, or a bare name to look up on PATH.
	string executable = "tippecanoe";
	//! argv[1..] for tippecanoe, minus the --output that names the target file.
	vector<string> arguments;
	//! Layer name; empty means "derive it from the output file name".
	string layer;
	//! Optional path to also receive the generated GeoJSON, for debugging.
	string keep_geojson;
	//! Let tippecanoe write to the terminal instead of capturing its output, so
	//! its progress meter is visible while a long build runs.
	bool verbose = false;
	//! Stream features as TCBF binary records instead of GeoJSON text. Decided
	//! at bind time: on by default when the binary advertises support and the
	//! query is compatible (no GeoJSON pass-through column, no KEEP_GEOJSON).
	bool use_binary = false;
	int coordinate_precision = DEFAULT_COORDINATE_PRECISION;

	vector<string> names;
	vector<LogicalType> sql_types;
	GeometryBinding geometry;
	//! Column promoted to the feature-level "id", if any.
	idx_t id_index = DConstants::INVALID_INDEX;
	//! Columns that become members of "properties".
	vector<idx_t> property_indexes;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MBTilesWriteBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MBTilesWriteBindData>();
		return executable == other.executable && arguments == other.arguments && layer == other.layer &&
		       keep_geojson == other.keep_geojson && verbose == other.verbose && use_binary == other.use_binary &&
		       coordinate_precision == other.coordinate_precision &&
		       names == other.names && sql_types == other.sql_types &&
		       geometry.encoding == other.geometry.encoding && geometry.geometry_index == other.geometry.geometry_index &&
		       geometry.longitude_index == other.geometry.longitude_index &&
		       geometry.latitude_index == other.geometry.latitude_index && id_index == other.id_index &&
		       property_indexes == other.property_indexes;
	}
};

//===--------------------------------------------------------------------===//
// Option parsing
//===--------------------------------------------------------------------===//

static const Value &SingleValue(const string &name, const vector<Value> &values) {
	if (values.size() != 1) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s expects exactly one argument", name);
	}
	return values[0];
}

static string StringOption(ClientContext &context, const string &name, const vector<Value> &values) {
	auto &value = SingleValue(name, values);
	if (value.IsNull()) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s cannot be NULL", name);
	}
	return value.CastAs(context, LogicalType::VARCHAR).GetValue<string>();
}

static int64_t IntegerOption(ClientContext &context, const string &name, const vector<Value> &values) {
	auto &value = SingleValue(name, values);
	if (value.IsNull()) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s cannot be NULL", name);
	}
	return value.CastAs(context, LogicalType::BIGINT).GetValue<int64_t>();
}

static double DoubleOption(ClientContext &context, const string &name, const vector<Value> &values) {
	auto &value = SingleValue(name, values);
	if (value.IsNull()) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s cannot be NULL", name);
	}
	return value.CastAs(context, LogicalType::DOUBLE).GetValue<double>();
}

static bool BooleanOption(ClientContext &context, const string &name, const vector<Value> &values) {
	// A bare option, as in (FORMAT mbtiles, DROP_DENSEST_AS_NEEDED), means true.
	if (values.empty()) {
		return true;
	}
	auto &value = SingleValue(name, values);
	if (value.IsNull()) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s cannot be NULL", name);
	}
	return value.CastAs(context, LogicalType::BOOLEAN).GetValue<bool>();
}

//! Renders an integer argument as "--flag=value".
static string FlagWithValue(const string &flag, int64_t value) {
	return flag + "=" + to_string(value);
}

//===--------------------------------------------------------------------===//
// Geometry resolution
//===--------------------------------------------------------------------===//

static idx_t FindColumn(const vector<string> &names, const string &needle) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (StringUtil::CIEquals(names[i], needle)) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

static idx_t FindColumnOrThrow(const vector<string> &names, const string &needle, const string &option) {
	const auto index = FindColumn(names, needle);
	if (index == DConstants::INVALID_INDEX) {
		throw BinderException("COPY ... (FORMAT mbtiles) option %s refers to column \"%s\", which is not in the "
		                      "result. Available columns: %s",
		                      option, needle, StringUtil::Join(names, ", "));
	}
	return index;
}

static bool IsGeometryType(const LogicalType &type) {
	return type.id() == LogicalTypeId::GEOMETRY || Geometry::IsSpatialGeometryType(type);
}

static bool IsNumericType(const LogicalType &type) {
	return type.IsNumeric();
}

//! Looks up an option under any of its accepted spellings.
static const vector<Value> *FindOption(const case_insensitive_map_t<vector<Value>> &options,
                                       const vector<string> &aliases, string &matched_name) {
	for (auto &alias : aliases) {
		auto entry = options.find(alias);
		if (entry != options.end()) {
			matched_name = alias;
			return &entry->second;
		}
	}
	return nullptr;
}

//! Decides where each feature's geometry comes from.
//!
//! Called twice with equivalent input - once from copy_to_select, which uses
//! the answer to insert the casts the writer relies on, and once from the bind,
//! which uses it to drive serialization. It must therefore be deterministic.
static GeometryBinding ResolveGeometry(ClientContext &context, const vector<string> &names,
                                       const vector<LogicalType> &types,
                                       const case_insensitive_map_t<vector<Value>> &options) {
	GeometryBinding result;

	string matched;
	const auto *geometry_option = FindOption(options, {"geometry_column", "geometry"}, matched);
	const auto *longitude_option = FindOption(options, {"lon", "longitude"}, matched);
	const string longitude_name = longitude_option ? StringOption(context, "LON", *longitude_option) : string();
	const auto *latitude_option = FindOption(options, {"lat", "latitude"}, matched);
	const string latitude_name = latitude_option ? StringOption(context, "LAT", *latitude_option) : string();

	if (static_cast<bool>(longitude_option) != static_cast<bool>(latitude_option)) {
		throw BinderException("COPY ... (FORMAT mbtiles) options LON and LAT must be given together");
	}

	// 1. An explicitly named geometry column wins.
	if (geometry_option) {
		const auto name = StringOption(context, "GEOMETRY_COLUMN", *geometry_option);
		const auto index = FindColumnOrThrow(names, name, "GEOMETRY_COLUMN");
		auto &type = types[index];
		if (IsGeometryType(type)) {
			result.encoding = GeometryEncoding::WKB;
		} else if (type.id() == LogicalTypeId::VARCHAR) {
			result.encoding = GeometryEncoding::GEOJSON_TEXT;
		} else {
			throw BinderException("COPY ... (FORMAT mbtiles): column \"%s\" has type %s, which cannot be used as a "
			                      "geometry. Use a GEOMETRY column, or a VARCHAR column holding GeoJSON geometry "
			                      "objects.",
			                      name, type.ToString());
		}
		result.geometry_index = index;
		return result;
	}

	// 2. An explicitly named longitude/latitude pair.
	if (longitude_option) {
		result.encoding = GeometryEncoding::LONGITUDE_LATITUDE;
		result.longitude_index = FindColumnOrThrow(names, longitude_name, "LON");
		result.latitude_index = FindColumnOrThrow(names, latitude_name, "LAT");
		for (auto index : {result.longitude_index, result.latitude_index}) {
			if (!IsNumericType(types[index])) {
				throw BinderException("COPY ... (FORMAT mbtiles): column \"%s\" has type %s, which is not numeric and "
				                      "so cannot be a coordinate",
				                      names[index], types[index].ToString());
			}
		}
		return result;
	}

	// 3. The first GEOMETRY column.
	for (idx_t i = 0; i < types.size(); i++) {
		if (IsGeometryType(types[i])) {
			result.encoding = GeometryEncoding::WKB;
			result.geometry_index = i;
			return result;
		}
	}

	// 4. A conventionally named pair of numeric coordinate columns.
	static const vector<string> LONGITUDE_NAMES = {"longitude", "lon", "lng", "long", "x"};
	static const vector<string> LATITUDE_NAMES = {"latitude", "lat", "y"};
	for (auto &candidate : LONGITUDE_NAMES) {
		const auto longitude_index = FindColumn(names, candidate);
		if (longitude_index == DConstants::INVALID_INDEX || !IsNumericType(types[longitude_index])) {
			continue;
		}
		for (auto &latitude_candidate : LATITUDE_NAMES) {
			const auto latitude_index = FindColumn(names, latitude_candidate);
			if (latitude_index == DConstants::INVALID_INDEX || !IsNumericType(types[latitude_index])) {
				continue;
			}
			result.encoding = GeometryEncoding::LONGITUDE_LATITUDE;
			result.longitude_index = longitude_index;
			result.latitude_index = latitude_index;
			return result;
		}
	}

	// 5. A conventionally named VARCHAR column holding GeoJSON.
	static const vector<string> GEOJSON_NAMES = {"geometry", "geom", "geojson"};
	for (auto &candidate : GEOJSON_NAMES) {
		const auto index = FindColumn(names, candidate);
		if (index != DConstants::INVALID_INDEX && types[index].id() == LogicalTypeId::VARCHAR) {
			result.encoding = GeometryEncoding::GEOJSON_TEXT;
			result.geometry_index = index;
			return result;
		}
	}

	throw BinderException(
	    "COPY ... (FORMAT mbtiles) could not find a geometry to write. Provide a GEOMETRY column, a VARCHAR "
	    "column of GeoJSON geometry objects, or longitude/latitude columns. The column can be named explicitly with "
	    "the GEOMETRY_COLUMN option, or with the LON and LAT options. Available columns: %s",
	    StringUtil::Join(names, ", "));
}

//===--------------------------------------------------------------------===//
// copy_to_select: normalize the columns the writer will read
//===--------------------------------------------------------------------===//

static vector<unique_ptr<Expression>> MBTilesWriteSelect(CopyToSelectInput &input) {
	vector<string> names;
	vector<LogicalType> types;
	names.reserve(input.select_list.size());
	types.reserve(input.select_list.size());
	for (auto &expr : input.select_list) {
		names.push_back(expr->GetName());
		types.push_back(expr->return_type);
	}

	const auto geometry = ResolveGeometry(input.context, names, types, input.options);

	vector<unique_ptr<Expression>> result;
	result.reserve(input.select_list.size());
	bool any_change = false;
	for (idx_t i = 0; i < input.select_list.size(); i++) {
		auto expr = std::move(input.select_list[i]);
		auto &type = expr->return_type;

		// Geometries produced by the older spatial extension are a BLOB with a
		// GEOMETRY alias; casting lifts them to the built-in type, whose values
		// are plain WKB.
		if (geometry.encoding == GeometryEncoding::WKB && i == geometry.geometry_index &&
		    Geometry::IsSpatialGeometryType(type)) {
			const auto name = expr->GetAlias();
			auto cast = BoundCastExpression::AddCastToType(input.context, std::move(expr), LogicalType::GEOMETRY(), false);
			cast->SetAlias(name);
			result.push_back(std::move(cast));
			any_change = true;
			continue;
		}

		// Coordinate columns are cast to DOUBLE once here rather than converted
		// per row in the writer.
		if (geometry.encoding == GeometryEncoding::LONGITUDE_LATITUDE &&
		    (i == geometry.longitude_index || i == geometry.latitude_index) && type.id() != LogicalTypeId::DOUBLE) {
			const auto name = expr->GetAlias();
			auto cast = BoundCastExpression::AddCastToType(input.context, std::move(expr), LogicalType::DOUBLE, false);
			cast->SetAlias(name);
			result.push_back(std::move(cast));
			any_change = true;
			continue;
		}

		result.push_back(std::move(expr));
	}

	if (!any_change) {
		return {};
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

static void ListMBTilesCopyOptions(ClientContext &, CopyOptionsInput &input) {
	static const char *WRITE_OPTIONS[] = {
	    "geometry_column", "geometry",   "lon",         "longitude", "lat",
	    "latitude",        "id_column",  "layer",       "name",      "description",
	    "attribution",     "minzoom",    "maxzoom",     "buffer",    "simplification",
	    "base_zoom",       "drop_rate",  "force",       "args",      "tippecanoe",
	    "read_parallel",   "keep_geojson",              "coordinate_precision",
	    "verbose",         "binary",    "drop_densest_as_needed",
	    "coalesce_densest_as_needed",    "extend_zooms_if_still_dropping",
	    "no_tile_size_limit",            "no_feature_limit",
	};
	for (auto &option : WRITE_OPTIONS) {
		input.options[option] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	}
}

//! tippecanoe names the layer after its input file, which is meaningless when
//! it is reading a pipe, so derive something recognisable from the target name.
//!
//! This runs on the path the user wrote, not the one the writer is handed:
//! overwriting an existing tileset makes DuckDB route the write through a
//! "tmp_"-prefixed file, and the layer name must not depend on that.
static string DeriveLayerName(const string &file_path) {
	auto base = file_path;
	const auto separator = base.find_last_of('/');
	if (separator != string::npos) {
		base = base.substr(separator + 1);
	}
	const auto dot = base.find_last_of('.');
	if (dot != string::npos && dot > 0) {
		base = base.substr(0, dot);
	}
	return base.empty() ? "features" : base;
}

static unique_ptr<FunctionData> MBTilesWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                                 const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto bind_data = make_uniq<MBTilesWriteBindData>();
	bind_data->names = names;
	bind_data->sql_types = sql_types;

	auto &options = input.info.options;
	bind_data->geometry = ResolveGeometry(context, names, sql_types, options);

	string id_column;
	// Options that map onto a tippecanoe flag but must be emitted after all
	// options are seen, because they interact.
	bool force = true;
	// Every feature is written on its own line, which is exactly what
	// tippecanoe's parallel reader needs, so this is on by default.
	bool read_parallel = true;
	// tippecanoe otherwise aborts the whole tileset when a single tile exceeds
	// 500 KB or 200k features. Lifting the caps is what lets a large, dense
	// extract finish; the cost is that individual tiles can grow past what some
	// renderers and CDNs will serve.
	bool no_tile_size_limit = true;
	bool no_feature_limit = true;
	//! BINARY option: unset means "probe the binary and use TCBF if it can".
	bool binary_option = true;
	bool binary_forced = false;
	vector<string> extra_arguments;

	for (auto &entry : options) {
		const auto option = StringUtil::Lower(entry.first);
		auto &values = entry.second;

		// Consumed by ResolveGeometry above.
		if (option == "geometry_column" || option == "geometry" || option == "lon" || option == "longitude" ||
		    option == "lat" || option == "latitude") {
			continue;
		}

		if (option == "id_column") {
			id_column = StringOption(context, "ID_COLUMN", values);
		} else if (option == "coordinate_precision") {
			const auto precision = IntegerOption(context, "COORDINATE_PRECISION", values);
			if (precision > 17) {
				throw BinderException("COPY ... (FORMAT mbtiles): COORDINATE_PRECISION must be at most 17; use a "
				                      "negative value for full round-trip precision");
			}
			bind_data->coordinate_precision = NumericCast<int>(precision);
		} else if (option == "tippecanoe") {
			bind_data->executable = StringOption(context, "TIPPECANOE", values);
		} else if (option == "keep_geojson") {
			bind_data->keep_geojson = StringOption(context, "KEEP_GEOJSON", values);
		} else if (option == "verbose") {
			bind_data->verbose = BooleanOption(context, "VERBOSE", values);
		} else if (option == "binary") {
			binary_option = BooleanOption(context, "BINARY", values);
			binary_forced = true;
		} else if (option == "layer") {
			bind_data->layer = StringOption(context, "LAYER", values);
		} else if (option == "name") {
			extra_arguments.push_back("--name=" + StringOption(context, "NAME", values));
		} else if (option == "description") {
			extra_arguments.push_back("--description=" + StringOption(context, "DESCRIPTION", values));
		} else if (option == "attribution") {
			extra_arguments.push_back("--attribution=" + StringOption(context, "ATTRIBUTION", values));
		} else if (option == "minzoom") {
			extra_arguments.push_back(FlagWithValue("--minimum-zoom", IntegerOption(context, "MINZOOM", values)));
		} else if (option == "maxzoom") {
			// Accepts a number, or "g"/"auto" for tippecanoe's own guess.
			auto &value = SingleValue("MAXZOOM", values);
			const auto text = value.IsNull() ? string() : value.CastAs(context, LogicalType::VARCHAR).GetValue<string>();
			if (StringUtil::CIEquals(text, "g") || StringUtil::CIEquals(text, "auto")) {
				extra_arguments.push_back("--maximum-zoom=g");
			} else {
				extra_arguments.push_back(FlagWithValue("--maximum-zoom", IntegerOption(context, "MAXZOOM", values)));
			}
		} else if (option == "buffer") {
			extra_arguments.push_back(FlagWithValue("--buffer", IntegerOption(context, "BUFFER", values)));
		} else if (option == "base_zoom") {
			extra_arguments.push_back(FlagWithValue("--base-zoom", IntegerOption(context, "BASE_ZOOM", values)));
		} else if (option == "simplification") {
			extra_arguments.push_back("--simplification=" +
			                          to_string(DoubleOption(context, "SIMPLIFICATION", values)));
		} else if (option == "drop_rate") {
			extra_arguments.push_back("--drop-rate=" + to_string(DoubleOption(context, "DROP_RATE", values)));
		} else if (option == "drop_densest_as_needed") {
			if (BooleanOption(context, "DROP_DENSEST_AS_NEEDED", values)) {
				extra_arguments.push_back("--drop-densest-as-needed");
			}
		} else if (option == "coalesce_densest_as_needed") {
			if (BooleanOption(context, "COALESCE_DENSEST_AS_NEEDED", values)) {
				extra_arguments.push_back("--coalesce-densest-as-needed");
			}
		} else if (option == "extend_zooms_if_still_dropping") {
			if (BooleanOption(context, "EXTEND_ZOOMS_IF_STILL_DROPPING", values)) {
				extra_arguments.push_back("--extend-zooms-if-still-dropping");
			}
		} else if (option == "no_tile_size_limit") {
			no_tile_size_limit = BooleanOption(context, "NO_TILE_SIZE_LIMIT", values);
		} else if (option == "no_feature_limit") {
			no_feature_limit = BooleanOption(context, "NO_FEATURE_LIMIT", values);
		} else if (option == "force") {
			force = BooleanOption(context, "FORCE", values);
		} else if (option == "read_parallel") {
			read_parallel = BooleanOption(context, "READ_PARALLEL", values);
		} else if (option == "args") {
			// Escape hatch: anything tippecanoe accepts, passed through verbatim.
			for (auto &value : values) {
				if (value.IsNull()) {
					throw BinderException("COPY ... (FORMAT mbtiles): ARGS must not contain NULL");
				}
				auto &type = value.type();
				if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
					for (auto &element : ListValue::GetChildren(value)) {
						extra_arguments.push_back(element.CastAs(context, LogicalType::VARCHAR).GetValue<string>());
					}
				} else {
					extra_arguments.push_back(value.CastAs(context, LogicalType::VARCHAR).GetValue<string>());
				}
			}
		} else {
			throw BinderException("Unknown option for COPY ... TO ... (FORMAT mbtiles): \"%s\"", entry.first);
		}
	}

	// Prepended so that anything the user passes through ARGS is seen later and
	// can still win where tippecanoe takes the last occurrence of a flag.
	if (no_feature_limit) {
		extra_arguments.insert(extra_arguments.begin(), "--no-feature-limit");
	}
	if (no_tile_size_limit) {
		extra_arguments.insert(extra_arguments.begin(), "--no-tile-size-limit");
	}
	if (read_parallel) {
		extra_arguments.insert(extra_arguments.begin(), "--read-parallel");
	}
	if (force) {
		extra_arguments.insert(extra_arguments.begin(), "--force");
	}

	if (!id_column.empty()) {
		bind_data->id_index = FindColumnOrThrow(names, id_column, "ID_COLUMN");
	}

	if (bind_data->layer.empty()) {
		bind_data->layer = DeriveLayerName(input.info.file_path);
	}

	// Everything that is not part of the geometry becomes a feature property.
	for (idx_t i = 0; i < names.size(); i++) {
		if (i == bind_data->geometry.geometry_index || i == bind_data->geometry.longitude_index ||
		    i == bind_data->geometry.latitude_index || i == bind_data->id_index) {
			continue;
		}
		bind_data->property_indexes.push_back(i);
	}

	// The binary protocol cannot carry a pass-through GeoJSON text column, and
	// KEEP_GEOJSON is defined as "a copy of the GeoJSON that was sent".
	const bool binary_compatible =
	    bind_data->geometry.encoding != GeometryEncoding::GEOJSON_TEXT && bind_data->keep_geojson.empty();
	if (binary_option && binary_compatible) {
		if (binary_forced) {
			// Explicit BINARY true skips the probe; a binary without TCBF
			// support will then fail with its own "malformed input" error.
			bind_data->use_binary = true;
		} else {
			bind_data->use_binary = TippecanoeSupportsTCBF(bind_data->executable);
		}
	} else if (binary_forced && binary_option && !binary_compatible) {
		throw BinderException("COPY ... (FORMAT mbtiles): BINARY true cannot be combined with a GeoJSON text "
		                      "geometry column or KEEP_GEOJSON");
	}

	bind_data->arguments = std::move(extra_arguments);
	return std::move(bind_data);
}

//===--------------------------------------------------------------------===//
// Global and local state
//===--------------------------------------------------------------------===//

struct MBTilesWriteGlobalState : public GlobalFunctionData {
	unique_ptr<TippecanoeProcess> process;
	unique_ptr<FileHandle> geojson_copy;
	//! Held across both writes so the debugging copy is byte-for-byte what
	//! tippecanoe was fed, in the same order, even when threads interleave.
	mutex flush_lock;

	void Flush(string &buffer) {
		if (buffer.empty()) {
			return;
		}
		{
			lock_guard<mutex> guard(flush_lock);
			if (geojson_copy) {
				geojson_copy->Write(const_cast<char *>(buffer.data()), // NOLINT: DuckDB API takes void *
				                    buffer.size());
			}
			process->Write(buffer.data(), buffer.size());
		}
		buffer.clear();
	}
};

struct MBTilesWriteLocalState : public LocalFunctionData {
	string buffer;
	//! Binary mode only: features accumulated in the current TCBF block, and
	//! scratch space reused across rows.
	uint64_t block_features = 0;
	TCBFScratch scratch;
};

static unique_ptr<GlobalFunctionData> MBTilesWriteInitializeGlobal(ClientContext &context, FunctionData &bind_data_p,
                                                                   const string &file_path) {
	auto &bind_data = bind_data_p.Cast<MBTilesWriteBindData>();
	auto result = make_uniq<MBTilesWriteGlobalState>();

	if (!bind_data.keep_geojson.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		result->geojson_copy =
		    fs.OpenFile(bind_data.keep_geojson, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	}

	auto arguments = bind_data.arguments;
	arguments.push_back("--output=" + file_path);
	arguments.push_back("--layer=" + bind_data.layer);

	result->process = make_uniq<TippecanoeProcess>(bind_data.executable, arguments, !bind_data.verbose);

	if (bind_data.use_binary) {
		// The magic is what routes tippecanoe's reader to the TCBF path.
		string magic;
		WriteTCBFMagic(magic);
		result->process->Write(magic.data(), magic.size());
	}
	return std::move(result);
}

static unique_ptr<LocalFunctionData> MBTilesWriteInitializeLocal(ExecutionContext &, FunctionData &bind_data_p) {
	auto &bind_data = bind_data_p.Cast<MBTilesWriteBindData>();
	auto result = make_uniq<MBTilesWriteLocalState>();
	result->buffer.reserve(FLUSH_THRESHOLD_BYTES + (FLUSH_THRESHOLD_BYTES / 4));
	if (bind_data.use_binary) {
		BeginTCBFBlock(result->buffer);
	}
	return std::move(result);
}

//! Seals the current TCBF block and hands it to tippecanoe, then starts the
//! next block. A block that gathered no features is discarded.
static void FlushTCBFBlock(MBTilesWriteGlobalState &gstate, MBTilesWriteLocalState &lstate) {
	if (FinishTCBFBlock(lstate.buffer, lstate.block_features)) {
		gstate.Flush(lstate.buffer);
	} else {
		lstate.buffer.clear();
	}
	BeginTCBFBlock(lstate.buffer);
	lstate.block_features = 0;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

//! Appends text that must not introduce a line break.
//!
//! tippecanoe's parallel reader divides the stream at newlines, so a feature
//! that spanned two lines would be split down the middle. Every byte this
//! extension generates is newline-free by construction, but a pass-through
//! GeoJSON column can hold pretty-printed text, so fold its line breaks into
//! spaces. Whitespace between JSON tokens carries no meaning, and the only
//! string inside a geometry object is its "type", which never contains one.
static void AppendSingleLine(const char *data, idx_t size, string &out) {
	idx_t prefix = 0;
	while (prefix < size && data[prefix] != '\n' && data[prefix] != '\r') {
		prefix++;
	}
	if (prefix == size) {
		out.append(data, size);
		return;
	}
	out.append(data, prefix);
	for (idx_t i = prefix; i < size; i++) {
		const auto c = data[i];
		out += (c == '\n' || c == '\r') ? ' ' : c;
	}
}

static void WriteGeometry(const MBTilesWriteBindData &bind_data, DataChunk &chunk, idx_t row, string &out) {
	auto &geometry = bind_data.geometry;
	switch (geometry.encoding) {
	case GeometryEncoding::WKB: {
		auto &source = chunk.data[geometry.geometry_index];
		if (!FlatVector::Validity(source).RowIsValid(row)) {
			out += "null";
			return;
		}
		auto &blob = FlatVector::GetData<string_t>(source)[row];
		WriteWKBAsGeoJSON(const_data_ptr_cast(blob.GetData()), blob.GetSize(), bind_data.coordinate_precision, out);
		return;
	}
	case GeometryEncoding::GEOJSON_TEXT: {
		auto &source = chunk.data[geometry.geometry_index];
		if (!FlatVector::Validity(source).RowIsValid(row)) {
			out += "null";
			return;
		}
		auto &text = FlatVector::GetData<string_t>(source)[row];
		if (text.GetSize() == 0) {
			out += "null";
			return;
		}
		AppendSingleLine(text.GetData(), text.GetSize(), out);
		return;
	}
	case GeometryEncoding::LONGITUDE_LATITUDE: {
		auto &longitude = chunk.data[geometry.longitude_index];
		auto &latitude = chunk.data[geometry.latitude_index];
		if (!FlatVector::Validity(longitude).RowIsValid(row) || !FlatVector::Validity(latitude).RowIsValid(row)) {
			out += "null";
			return;
		}
		WritePointAsGeoJSON(FlatVector::GetData<double>(longitude)[row], FlatVector::GetData<double>(latitude)[row],
		                    bind_data.coordinate_precision, out);
		return;
	}
	default:
		throw InternalException("Unhandled geometry encoding in the mbtiles writer");
	}
}

static void WriteFeature(const MBTilesWriteBindData &bind_data, DataChunk &chunk, idx_t row, string &out) {
	out += R"({"type":"Feature")";

	if (bind_data.id_index != DConstants::INVALID_INDEX) {
		auto &source = chunk.data[bind_data.id_index];
		if (FlatVector::Validity(source).RowIsValid(row)) {
			out += R"(,"id":)";
			WriteVectorEntryAsJSON(source, row, out);
		}
	}

	out += R"(,"geometry":)";
	WriteGeometry(bind_data, chunk, row, out);

	out += R"(,"properties":{)";
	bool first = true;
	for (auto index : bind_data.property_indexes) {
		auto &source = chunk.data[index];
		// NULL properties are left out entirely; tippecanoe treats an absent
		// attribute and a null one the same way, and omitting them is smaller.
		if (!FlatVector::Validity(source).RowIsValid(row)) {
			continue;
		}
		if (!first) {
			out += ',';
		}
		first = false;
		WriteJSONString(bind_data.names[index], out);
		out += ':';
		WriteVectorEntryAsJSON(source, row, out);
	}
	out += "}}\n";
}

static void MBTilesWriteSink(ExecutionContext &, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                             LocalFunctionData &lstate_p, DataChunk &input) {
	auto &bind_data = bind_data_p.Cast<MBTilesWriteBindData>();
	auto &gstate = gstate_p.Cast<MBTilesWriteGlobalState>();
	auto &lstate = lstate_p.Cast<MBTilesWriteLocalState>();

	// The writer reads vectors positionally, so make every one of them flat.
	input.Flatten();

	if (bind_data.use_binary) {
		const auto wkb_index = bind_data.geometry.encoding == GeometryEncoding::WKB ? bind_data.geometry.geometry_index
		                                                                            : DConstants::INVALID_INDEX;
		for (idx_t row = 0; row < input.size(); row++) {
			if (WriteTCBFFeature(input, row, wkb_index, bind_data.geometry.longitude_index,
			                     bind_data.geometry.latitude_index, bind_data.id_index, bind_data.property_indexes,
			                     bind_data.names, lstate.scratch, lstate.buffer)) {
				lstate.block_features++;
			}
		}
		if (lstate.buffer.size() >= FLUSH_THRESHOLD_BYTES) {
			FlushTCBFBlock(gstate, lstate);
		}
		return;
	}

	for (idx_t row = 0; row < input.size(); row++) {
		WriteFeature(bind_data, input, row, lstate.buffer);
	}
	if (lstate.buffer.size() >= FLUSH_THRESHOLD_BYTES) {
		gstate.Flush(lstate.buffer);
	}
}

static void MBTilesWriteCombine(ExecutionContext &, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                                LocalFunctionData &lstate_p) {
	auto &bind_data = bind_data_p.Cast<MBTilesWriteBindData>();
	auto &gstate = gstate_p.Cast<MBTilesWriteGlobalState>();
	auto &lstate = lstate_p.Cast<MBTilesWriteLocalState>();
	if (bind_data.use_binary) {
		if (FinishTCBFBlock(lstate.buffer, lstate.block_features)) {
			gstate.Flush(lstate.buffer);
		}
		lstate.buffer.clear();
		lstate.block_features = 0;
		return;
	}
	gstate.Flush(lstate.buffer);
}

static void MBTilesWriteFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<MBTilesWriteGlobalState>();
	if (gstate.geojson_copy) {
		gstate.geojson_copy->Sync();
		gstate.geojson_copy.reset();
	}
	// Closes tippecanoe's input and waits for it to finish building the tileset.
	gstate.process->Finish();
}

static CopyFunctionExecutionMode MBTilesWriteExecutionMode(bool preserve_insertion_order, bool) {
	// tippecanoe sorts every feature into tiles regardless of arrival order, so
	// the order only matters for its --preserve-input-order mode, which a user
	// can reach through ARGS. Honour the session setting so that stays usable.
	if (preserve_insertion_order) {
		return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
	}
	return CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

//! Both tileset containers are written by the same machinery: the GeoJSON we
//! generate is identical, and tippecanoe decides the container from the name of
//! the output file.
static CopyFunction MakeTilesetCopyFunction(const string &name) {
	CopyFunction function(name);
	function.extension = name;

	function.copy_to_select = MBTilesWriteSelect;
	function.copy_options = ListMBTilesCopyOptions;
	function.copy_to_bind = MBTilesWriteBind;
	function.copy_to_initialize_global = MBTilesWriteInitializeGlobal;
	function.copy_to_initialize_local = MBTilesWriteInitializeLocal;
	function.copy_to_sink = MBTilesWriteSink;
	function.copy_to_combine = MBTilesWriteCombine;
	function.copy_to_finalize = MBTilesWriteFinalize;
	function.execution_mode = MBTilesWriteExecutionMode;

	return function;
}

CopyFunction GetMBTilesCopyFunction() {
	return MakeTilesetCopyFunction("mbtiles");
}

CopyFunction GetPMTilesCopyFunction() {
	return MakeTilesetCopyFunction("pmtiles");
}

} // namespace duckdb
