#define DUCKDB_EXTENSION_MAIN

#include "tippecanoe_extension.hpp"

#include "mbtiles_copy.hpp"
#include "mbtiles_reader.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

//! Lets `SELECT * FROM 'tiles.mbtiles'` and the .pmtiles equivalent work, by
//! rewriting a bare tileset path into the matching reader call.
static unique_ptr<TableRef> TilesetReplacementScan(ClientContext &, ReplacementScanInput &input,
                                                   optional_ptr<ReplacementScanData>) {
	auto table_name = ReplacementScan::GetFullPath(input);
	string function_name;
	if (ReplacementScan::CanReplace(table_name, {"mbtiles"})) {
		function_name = "read_mbtiles";
	} else if (ReplacementScan::CanReplace(table_name, {"pmtiles"})) {
		function_name = "read_pmtiles";
	} else {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>(function_name, std::move(children));
	return std::move(table_function);
}

static void LoadInternal(ExtensionLoader &loader) {
	// Registering the file extensions means COPY ... TO 'tiles.mbtiles' and
	// 'tiles.pmtiles' both work without an explicit FORMAT.
	loader.RegisterFunction(GetMBTilesCopyFunction());
	loader.RegisterFunction(GetPMTilesCopyFunction());

	loader.RegisterFunction(GetReadMBTilesFunction());
	loader.RegisterFunction(GetReadPMTilesFunction());
	loader.RegisterFunction(GetReadMBTilesTilesFunction());
	loader.RegisterFunction(GetReadPMTilesTilesFunction());

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(TilesetReplacementScan);
}

void TippecanoeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TippecanoeExtension::Name() {
	return "tippecanoe";
}

std::string TippecanoeExtension::Version() const {
#ifdef EXT_VERSION_TIPPECANOE
	return EXT_VERSION_TIPPECANOE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(tippecanoe, loader) {
	duckdb::LoadInternal(loader);
}
}
