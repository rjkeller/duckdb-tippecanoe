//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// wkb_reader.hpp
//
// Bounds-checked cursor over a WKB blob, shared by the GeoJSON writer and the
// TCBF binary feature writer.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"

#include <cstring>

namespace duckdb {

inline bool WKBHostIsLittleEndian() {
	const uint16_t probe = 1;
	uint8_t first_byte;
	memcpy(&first_byte, &probe, 1);
	return first_byte == 1;
}

inline uint32_t WKBByteSwap32(uint32_t value) {
	return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) | ((value & 0x00FF0000u) >> 8) |
	       ((value & 0xFF000000u) >> 24);
}

inline uint64_t WKBByteSwap64(uint64_t value) {
	return ((value & 0x00000000000000FFull) << 56) | ((value & 0x000000000000FF00ull) << 40) |
	       ((value & 0x0000000000FF0000ull) << 24) | ((value & 0x00000000FF000000ull) << 8) |
	       ((value & 0x000000FF00000000ull) >> 8) | ((value & 0x0000FF0000000000ull) >> 24) |
	       ((value & 0x00FF000000000000ull) >> 40) | ((value & 0xFF00000000000000ull) >> 56);
}

//! Cursor over a WKB blob with bounds checking on every read.
struct WKBReader {
	WKBReader(const_data_ptr_t data, idx_t size)
	    : ptr(data), end(data + size), host_little_endian(WKBHostIsLittleEndian()) {
	}

	const_data_ptr_t ptr;
	const_data_ptr_t end;
	bool host_little_endian;
	//! Byte order of the geometry currently being read. Every geometry in a WKB
	//! blob carries its own marker, including nested ones.
	bool swap_bytes = false;

	void Require(idx_t bytes) const {
		if (static_cast<idx_t>(end - ptr) < bytes) {
			throw InvalidInputException("Malformed WKB geometry: expected %llu more byte(s) but the blob ended",
			                            static_cast<unsigned long long>(bytes));
		}
	}

	uint8_t ReadByte() {
		Require(1);
		return *ptr++;
	}

	uint32_t ReadUInt32() {
		Require(sizeof(uint32_t));
		uint32_t value;
		memcpy(&value, ptr, sizeof(value));
		ptr += sizeof(value);
		return swap_bytes ? WKBByteSwap32(value) : value;
	}

	double ReadDouble() {
		Require(sizeof(double));
		uint64_t bits;
		memcpy(&bits, ptr, sizeof(bits));
		ptr += sizeof(bits);
		if (swap_bytes) {
			bits = WKBByteSwap64(bits);
		}
		double value;
		memcpy(&value, &bits, sizeof(value));
		return value;
	}

	//! Reads a count and rejects values that cannot possibly be backed by the
	//! remaining bytes, so that a corrupt length cannot make us allocate wildly.
	uint32_t ReadCount(idx_t min_bytes_per_element) {
		const auto count = ReadUInt32();
		if (min_bytes_per_element > 0) {
			const auto remaining = static_cast<idx_t>(end - ptr);
			if (static_cast<idx_t>(count) > remaining / min_bytes_per_element) {
				throw InvalidInputException(
				    "Malformed WKB geometry: element count %u exceeds the %llu remaining byte(s)", count,
				    static_cast<unsigned long long>(remaining));
			}
		}
		return count;
	}
};

struct WKBGeometryHeader {
	GeometryType type;
	bool has_z;
	bool has_m;
};

inline WKBGeometryHeader ReadGeometryHeader(WKBReader &reader) {
	const auto byte_order = reader.ReadByte();
	if (byte_order > 1) {
		throw InvalidInputException("Malformed WKB geometry: invalid byte order marker %d", byte_order);
	}
	const bool little_endian = byte_order == 1;
	reader.swap_bytes = little_endian != reader.host_little_endian;

	auto code = reader.ReadUInt32();

	// EWKB (PostGIS) flags dimensionality in the high bits of the type code.
	bool has_z = (code & 0x80000000u) != 0;
	bool has_m = (code & 0x40000000u) != 0;
	const bool has_srid = (code & 0x20000000u) != 0;
	code &= 0x0FFFFFFFu;

	// ISO WKB - which is what DuckDB itself produces - encodes it in the
	// thousands digit instead: 1000 = Z, 2000 = M, 3000 = ZM.
	switch (code / 1000) {
	case 0:
		break;
	case 1:
		has_z = true;
		break;
	case 2:
		has_m = true;
		break;
	case 3:
		has_z = true;
		has_m = true;
		break;
	default:
		throw InvalidInputException("Malformed WKB geometry: unsupported geometry code %u", code);
	}
	code %= 1000;
	if (code < 1 || code > 7) {
		throw InvalidInputException("Malformed WKB geometry: unknown geometry type %u", code);
	}
	if (has_srid) {
		reader.ReadUInt32(); // SRID is ignored: coordinates are assumed WGS84.
	}
	return WKBGeometryHeader {static_cast<GeometryType>(code), has_z, has_m};
}

} // namespace duckdb
