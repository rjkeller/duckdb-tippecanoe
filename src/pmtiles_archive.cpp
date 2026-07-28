#include "tile_archive.hpp"

#include "mvt_decoder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

namespace duckdb {

namespace {

//! Fixed size of the PMTiles v3 header, in bytes.
constexpr idx_t PMTILES_HEADER_SIZE = 127;

//! Values of the compression fields in the header.
enum class PMTilesCompression : uint8_t {
	UNKNOWN = 0,
	NONE = 1,
	GZIP = 2,
	BROTLI = 3,
	ZSTD = 4,
};

struct PMTilesHeader {
	uint64_t root_offset;
	uint64_t root_length;
	uint64_t metadata_offset;
	uint64_t metadata_length;
	uint64_t leaf_offset;
	uint64_t leaf_length;
	uint64_t tile_data_offset;
	uint64_t tile_data_length;
	PMTilesCompression internal_compression;
	PMTilesCompression tile_compression;
	uint8_t min_zoom;
	uint8_t max_zoom;
};

//! One entry of a PMTiles directory.
//!
//! `run_length` of zero marks a pointer to a leaf directory rather than a
//! tile; anything greater means this entry covers that many consecutive tile
//! ids that all share the same bytes.
struct PMTilesEntry {
	uint64_t tile_id;
	uint64_t offset;
	uint64_t length;
	uint64_t run_length;
};

uint64_t ReadLE64(const_data_ptr_t data) {
	uint64_t value;
	memcpy(&value, data, sizeof(value));
	return value;
}

//! Cursor over the LEB128 varints PMTiles uses to serialize directories.
struct VarintReader {
	VarintReader(const_data_ptr_t data, idx_t size) : ptr(data), end(data + size) {
	}
	const_data_ptr_t ptr;
	const_data_ptr_t end;

	uint64_t Read() {
		uint64_t result = 0;
		uint32_t shift = 0;
		while (true) {
			if (ptr >= end) {
				throw InvalidInputException("Malformed PMTiles directory: truncated varint");
			}
			const uint8_t byte = *ptr++;
			if (shift > 63) {
				throw InvalidInputException("Malformed PMTiles directory: oversized varint");
			}
			result |= static_cast<uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) {
				return result;
			}
			shift += 7;
		}
	}
};

//! Maps a Hilbert-curve tile id back to its zoom and position.
//!
//! PMTiles numbers tiles along a Hilbert curve, zoom level after zoom level,
//! so the id first has to be reduced to an offset within its own zoom.
void TileIdToZXY(uint64_t tile_id, int32_t &zoom, int64_t &out_x, int64_t &out_y) {
	uint64_t accumulated = 0;
	for (int32_t z = 0; z < 32; z++) {
		const uint64_t tiles_at_zoom = uint64_t(1) << (2 * z);
		if (tile_id < accumulated + tiles_at_zoom) {
			uint64_t position = tile_id - accumulated;
			const uint64_t n = uint64_t(1) << z;
			uint64_t x = 0;
			uint64_t y = 0;
			// Standard Hilbert d2xy.
			for (uint64_t s = 1; s < n; s *= 2) {
				const uint64_t rx = 1 & (position / 2);
				const uint64_t ry = 1 & (position ^ rx);
				if (ry == 0) {
					if (rx == 1) {
						x = s - 1 - x;
						y = s - 1 - y;
					}
					const uint64_t swap = x;
					x = y;
					y = swap;
				}
				x += s * rx;
				y += s * ry;
				position /= 4;
			}
			zoom = z;
			out_x = NumericCast<int64_t>(x);
			out_y = NumericCast<int64_t>(y);
			return;
		}
		accumulated += tiles_at_zoom;
	}
	throw InvalidInputException("Malformed PMTiles archive: tile id %llu is out of range",
	                            static_cast<unsigned long long>(tile_id));
}

vector<PMTilesEntry> DeserializeDirectory(const string &bytes) {
	VarintReader reader(const_data_ptr_cast(bytes.c_str()), bytes.size());
	const auto count = reader.Read();
	if (count > (bytes.size() + 1) * 8) {
		throw InvalidInputException("Malformed PMTiles directory: implausible entry count");
	}
	vector<PMTilesEntry> entries(count);

	// Tile ids are stored as deltas.
	uint64_t last_id = 0;
	for (idx_t i = 0; i < count; i++) {
		last_id += reader.Read();
		entries[i].tile_id = last_id;
	}
	for (idx_t i = 0; i < count; i++) {
		entries[i].run_length = reader.Read();
	}
	for (idx_t i = 0; i < count; i++) {
		entries[i].length = reader.Read();
	}
	// An offset of zero means "directly after the previous entry", which is how
	// clustered archives avoid storing a monotonically rising list.
	for (idx_t i = 0; i < count; i++) {
		const auto raw = reader.Read();
		if (raw == 0 && i > 0) {
			entries[i].offset = entries[i - 1].offset + entries[i - 1].length;
		} else {
			entries[i].offset = raw - 1;
		}
	}
	return entries;
}

class PMTilesArchive : public TileArchive {
public:
	PMTilesArchive(ClientContext &context, const string &path) : path(path) {
		auto &fs = FileSystem::GetFileSystem(context);
		handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		if (!handle) {
			throw IOException("Failed to open \"%s\"", path);
		}
		ReadHeader();
		CollectEntries();
	}

	bool NextTile(TileRecord &record) override {
		while (cursor < entries.size()) {
			auto &entry = entries[cursor];
			// One entry can stand for a run of identical tiles; walk the run
			// before moving on.
			const auto tile_id = entry.tile_id + run_offset;
			const bool run_finished = ++run_offset >= MaxValue<uint64_t>(entry.run_length, 1);
			if (run_finished) {
				cursor++;
				run_offset = 0;
			}

			int32_t zoom;
			int64_t x, y;
			TileIdToZXY(tile_id, zoom, x, y);
			if (zoom_filter.IsValid() && NumericCast<idx_t>(zoom) != zoom_filter.GetIndex()) {
				continue;
			}
			if (entry.length == 0) {
				continue;
			}

			string raw;
			raw.resize(entry.length);
			handle->Read(&raw[0], entry.length, header.tile_data_offset + entry.offset);
			auto tile = DecompressBlock(raw, header.tile_compression);
			if (tile.empty()) {
				continue;
			}
			record.zoom = zoom;
			record.x = x;
			record.y = y;
			record.data = std::move(tile);
			return true;
		}
		return false;
	}

	string GetMetadataJSON() override {
		if (header.metadata_length == 0) {
			return string();
		}
		string raw;
		raw.resize(header.metadata_length);
		handle->Read(&raw[0], header.metadata_length, header.metadata_offset);
		return DecompressBlock(raw, header.internal_compression);
	}

	optional_idx GetMaxZoom() override {
		return NumericCast<idx_t>(header.max_zoom);
	}

	void SetZoomFilter(idx_t zoom) override {
		zoom_filter = zoom;
	}

private:
	void ReadHeader() {
		data_t raw[PMTILES_HEADER_SIZE];
		handle->Read(raw, PMTILES_HEADER_SIZE, 0);
		if (memcmp(raw, "PMTiles", 7) != 0) {
			throw InvalidInputException("\"%s\" is not a PMTiles archive", path);
		}
		if (raw[7] != 3) {
			throw InvalidInputException("\"%s\" is PMTiles version %d; only version 3 is supported", path,
			                            static_cast<int>(raw[7]));
		}
		header.root_offset = ReadLE64(raw + 8);
		header.root_length = ReadLE64(raw + 16);
		header.metadata_offset = ReadLE64(raw + 24);
		header.metadata_length = ReadLE64(raw + 32);
		header.leaf_offset = ReadLE64(raw + 40);
		header.leaf_length = ReadLE64(raw + 48);
		header.tile_data_offset = ReadLE64(raw + 56);
		header.tile_data_length = ReadLE64(raw + 64);
		header.internal_compression = static_cast<PMTilesCompression>(raw[97]);
		header.tile_compression = static_cast<PMTilesCompression>(raw[98]);
		header.min_zoom = raw[100];
		header.max_zoom = raw[101];
	}

	string DecompressBlock(const string &raw, PMTilesCompression compression) {
		switch (compression) {
		case PMTilesCompression::NONE:
			return raw;
		case PMTilesCompression::GZIP:
			return MaybeGunzipTile(const_data_ptr_cast(raw.c_str()), raw.size());
		case PMTilesCompression::UNKNOWN:
			// Sniff it: tippecanoe writes gzip, but the field is optional.
			return MaybeGunzipTile(const_data_ptr_cast(raw.c_str()), raw.size());
		default:
			throw NotImplementedException(
			    "\"%s\" uses %s compression, which this extension cannot read; only gzip and uncompressed are "
			    "supported",
			    path, compression == PMTilesCompression::BROTLI ? "brotli" : "zstd");
		}
	}

	string ReadDirectory(uint64_t offset, uint64_t length) {
		if (length == 0) {
			return string();
		}
		string raw;
		raw.resize(length);
		handle->Read(&raw[0], length, offset);
		return DecompressBlock(raw, header.internal_compression);
	}

	//! Walks the root directory and every leaf it points at, gathering the
	//! entries that actually address tiles.
	void CollectEntries() {
		const auto root = ReadDirectory(header.root_offset, header.root_length);
		if (root.empty()) {
			return;
		}
		for (auto &entry : DeserializeDirectory(root)) {
			if (entry.run_length > 0) {
				entries.push_back(entry);
				continue;
			}
			// A leaf pointer: read that directory and take its tile entries.
			const auto leaf = ReadDirectory(header.leaf_offset + entry.offset, entry.length);
			if (leaf.empty()) {
				continue;
			}
			for (auto &leaf_entry : DeserializeDirectory(leaf)) {
				// PMTiles only nests one level deep in practice; anything that
				// claims to nest further is skipped rather than followed.
				if (leaf_entry.run_length > 0) {
					entries.push_back(leaf_entry);
				}
			}
		}
	}

	string path;
	unique_ptr<FileHandle> handle;
	PMTilesHeader header {};
	vector<PMTilesEntry> entries;
	idx_t cursor = 0;
	uint64_t run_offset = 0;
	optional_idx zoom_filter;
};

} // namespace

unique_ptr<TileArchive> OpenPMTilesArchive(ClientContext &context, const string &path) {
	return make_uniq<PMTilesArchive>(context, path);
}

} // namespace duckdb
