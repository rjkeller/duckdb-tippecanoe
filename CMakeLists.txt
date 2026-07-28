cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME tippecanoe)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
include_directories(third_party/sqlite3)
# For tcbf.hpp, the binary feature protocol shared with the vendored tippecanoe.
include_directories(third_party/tippecanoe)

# Reading .mbtiles means reading SQLite, which DuckDB does not vendor, so the
# amalgamation is compiled straight into the extension. Trimmed to the parts a
# read-only tile reader needs.
set(SQLITE_DEFINITIONS
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_SHARED_CACHE=1
    SQLITE_USE_ALLOCA=1)

set(EXTENSION_SOURCES
    src/tippecanoe_extension.cpp
    src/geojson_writer.cpp
    src/tcbf_writer.cpp
    src/tippecanoe_process.cpp
    src/mbtiles_copy.cpp
    src/mvt_decoder.cpp
    src/mbtiles_reader.cpp
    src/pmtiles_archive.cpp
    third_party/sqlite3/sqlite3.c)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_definitions(${EXTENSION_NAME} PRIVATE ${SQLITE_DEFINITIONS})
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE ${SQLITE_DEFINITIONS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
