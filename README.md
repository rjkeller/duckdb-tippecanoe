# duckdb-tippecanoe

A DuckDB extension that reads and writes vector tilesets, in both `.mbtiles`
and `.pmtiles`.

```sql
LOAD tippecanoe;

-- Write
COPY (SELECT geom, name, population FROM cities)
TO 'cities.mbtiles' (FORMAT mbtiles, MAXZOOM 12);

COPY (SELECT geom, name, population FROM cities)
TO 'cities.pmtiles' (FORMAT pmtiles, MAXZOOM 12);

-- Read
SELECT * FROM 'cities.mbtiles';
SELECT * FROM 'cities.pmtiles';
```

The two containers are written by the same path — tippecanoe picks the format
from the output file name — and hold the same tiles, so which one you choose is
a packaging decision. PMTiles is a single file designed to be served from
object storage by HTTP range request; MBTiles is a SQLite database. In practice
PMTiles also comes out noticeably smaller: 145 KB against 209 KB for the same
US state boundaries.

Each row is rendered as a [newline-delimited GeoJSON][ndjson] `Feature` and
streamed into [tippecanoe][], which builds the vector tileset. Nothing is
staged on disk in between: the features go straight down a pipe into
tippecanoe's standard input while the query is still running.

Piping rather than staging a `.geojsonl` file first is a measured choice, not
just a tidy one. On 14M parcel polygons (9 GB of GeoJSON), tippecanoe took
330.0s whether it read a seekable file or standard input — reading is not the
bottleneck, the sort/tile phase is. Piping finished in 344.8s against 370.5s
for dump-then-load, because roughly two thirds of the time spent generating
GeoJSON disappears inside tippecanoe's read phase instead of being paid up
front. The pipe also avoids writing and re-reading the intermediate entirely,
which at full-dataset scale would be about 106 GB.

[tippecanoe]: https://github.com/felt/tippecanoe
[ndjson]: https://github.com/mapbox/tippecanoe#input-files-and-layer-names

## Requirements

- DuckDB v1.5.5
- `tippecanoe` on your `PATH` (`brew install tippecanoe`), or named explicitly
  with the `TIPPECANOE` option
- macOS or Linux. Windows is not supported, because the extension spawns
  tippecanoe as a child process.

The `spatial` extension is **not** required. DuckDB v1.5 has a built-in
`GEOMETRY` type, and this extension converts its WKB representation to GeoJSON
itself.

## Building

None of the build dependencies are tracked here, so fetch them first:

```sh
git clone --depth 1 --branch v1.5.5 https://github.com/duckdb/duckdb.git
git clone --depth 1 https://github.com/duckdb/extension-ci-tools.git

# The patched tippecanoe. Its tcbf.hpp is on the extension's include path, so
# this is a build dependency and not only a runtime one.
git clone --depth 1 --branch duckdb-tcbf \
	https://github.com/rjkeller/tippecanoe.git third_party/tippecanoe

# The SQLite amalgamation, compiled into the extension to read .mbtiles.
mkdir -p third_party/sqlite3
curl -O https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
unzip -j sqlite-amalgamation-3460100.zip \
	'sqlite-amalgamation-3460100/sqlite3.[ch]' -d third_party/sqlite3
rm sqlite-amalgamation-3460100.zip
```

Then build:

```sh
GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=18 make release
```

This produces `build/release/extension/tippecanoe/tippecanoe.duckdb_extension`
along with a `duckdb` binary that has the extension already linked in.

To use the extension from a stock DuckDB, load it unsigned:

```sh
duckdb -unsigned
```
```sql
LOAD '/path/to/tippecanoe.duckdb_extension';
```

## Finding the geometry

The extension picks the geometry for each feature using the first rule that
matches:

1. The column named by `GEOMETRY_COLUMN`.
2. The columns named by `LON` and `LAT`.
3. The first `GEOMETRY` column.
4. A numeric pair named `longitude`/`lon`/`lng`/`long`/`x` and
   `latitude`/`lat`/`y`.
5. A `VARCHAR` column named `geometry`, `geom` or `geojson`, holding GeoJSON
   geometry objects that are passed through as-is.

If none match, the bind fails with a message listing the available columns.

Whatever is left over becomes the feature's `properties`. `NULL` properties are
omitted rather than written as `null`, which is how tippecanoe treats a missing
attribute anyway.

## Options

### Choosing columns

| Option | Meaning |
| --- | --- |
| `GEOMETRY_COLUMN 'geom'` | Use this column as the geometry |
| `LON 'x'`, `LAT 'y'` | Build Points from these two columns (both required) |
| `ID_COLUMN 'fid'` | Promote this column to the GeoJSON feature `id` |
| `COORDINATE_PRECISION 7` | Decimals kept per coordinate; negative means full round-trip precision |

### Passed to tippecanoe

| Option | tippecanoe flag |
| --- | --- |
| `LAYER 'roads'` | `--layer` (defaults to the output file's stem) |
| `NAME`, `DESCRIPTION`, `ATTRIBUTION` | `--name`, `--description`, `--attribution` |
| `MINZOOM 0`, `MAXZOOM 14` | `--minimum-zoom`, `--maximum-zoom` |
| `MAXZOOM 'g'` | `--maximum-zoom=g`, tippecanoe's own guess |
| `BUFFER 5`, `SIMPLIFICATION 10.0` | `--buffer`, `--simplification` |
| `BASE_ZOOM`, `DROP_RATE` | `--base-zoom`, `--drop-rate` |
| `DROP_DENSEST_AS_NEEDED` | `--drop-densest-as-needed` |
| `COALESCE_DENSEST_AS_NEEDED` | `--coalesce-densest-as-needed` |
| `EXTEND_ZOOMS_IF_STILL_DROPPING` | `--extend-zooms-if-still-dropping` |
| `NO_TILE_SIZE_LIMIT false`, `NO_FEATURE_LIMIT false` | restores tippecanoe's caps, which are lifted by default |
| `READ_PARALLEL false` | drops the `--read-parallel` that is passed by default |
| `FORCE false` | drops the `--force` that is passed by default |
| `ARGS ['--cluster-distance=10']` | anything else, verbatim and last |

Boolean options may be written bare — `(FORMAT mbtiles, DROP_DENSEST_AS_NEEDED)`.

### Debugging

`KEEP_GEOJSON 'features.geojsonl'` writes a copy of everything sent to
tippecanoe. This is the quickest way to see what the extension actually
produced:

```sql
COPY (SELECT geom, name FROM roads)
TO 'roads.mbtiles' (FORMAT mbtiles, KEEP_GEOJSON 'roads.geojsonl');
```

`VERBOSE true` lets tippecanoe write straight to the terminal instead of having
its output captured, so its progress meter is visible while a long build runs.
The trade-off is that a failure can no longer quote tippecanoe's output back
inside the DuckDB error — you read it off the screen instead. Worth setting for
anything that takes more than a minute:

```sql
COPY (SELECT geom, apn FROM parcels)
TO 'parcels.mbtiles' (FORMAT mbtiles, MAXZOOM 15, VERBOSE true);
```

`TIPPECANOE '/opt/homebrew/bin/tippecanoe'` selects a specific binary.

`BINARY false` disables the binary feature protocol (see Performance) and
streams GeoJSON text even to a tippecanoe that could accept the binary form.

## Examples

Points from plain longitude/latitude columns, with a zoom range:

```sql
COPY (SELECT lon, lat, name, population FROM cities)
TO 'cities.mbtiles' (FORMAT mbtiles, LAYER 'cities', MINZOOM 0, MAXZOOM 10);
```

Reading Parquet and thinning dense areas:

```sql
COPY (SELECT ST_GeomFromWKB(wkb) AS geom, * EXCLUDE (wkb)
      FROM 'buildings/*.parquet')
TO 'buildings.mbtiles'
   (FORMAT mbtiles, MAXZOOM 'g', DROP_DENSEST_AS_NEEDED, LAYER 'buildings');
```

Passing flags this extension does not model:

```sql
COPY (SELECT geom, kind FROM features)
TO 'features.mbtiles'
   (FORMAT mbtiles, ARGS ['--cluster-distance=8', '--accumulate-attribute=count:sum']);
```

## Reading tilesets back

```sql
SELECT * FROM 'cities.mbtiles';
```

Each row is one feature, with a real `GEOMETRY` column in WGS84. The tileset's
own attributes are promoted to columns, read from the `vector_layers` metadata
tippecanoe writes, so no tile has to be decoded to work out the schema:

| geom | layer | zoom | tile_x | tile_y | feature_id | *…attributes…* |
| --- | --- | --- | --- | --- | --- | --- |

`tile_x` and `tile_y` are XYZ addresses. (The `.mbtiles` format stores rows
bottom-up in TMS order; the conversion happens on the way out.)

```sql
-- A specific zoom, rather than the default
SELECT * FROM read_mbtiles('cities.mbtiles', zoom := 8);

-- A single layer out of a multi-layer tileset
SELECT * FROM read_mbtiles('cities.mbtiles', layer := 'roads');

-- The stored tiles themselves, decompressed but still encoded
SELECT zoom, tile_x, tile_y, octet_length(tile_data)
FROM read_mbtiles_tiles('cities.mbtiles');
```

`read_pmtiles()` and `read_pmtiles_tiles()` are the same functions for PMTiles
archives, and take the same parameters. The two formats share nothing below the
surface — one is a SQLite database addressed in TMS rows, the other a single
archive addressed along a Hilbert curve — but they are decoded into identical
rows, which the test suite checks by comparing them geometry and all.

Nothing needs installing to read either: SQLite is compiled into this
extension, and the PMTiles archive and vector tiles are decoded in-process.

### What you get back is not what you put in

A tileset is a rendering of your data, not a copy of it, and reading one back
inherits every transformation tippecanoe applied:

- **Features repeat.** Tiles are buffered, so a feature near an edge is stored
  in the neighbouring tile too. Deduplicate on `feature_id`, or on an
  attribute, when you want one row per source feature.
- **Large geometries arrive in pieces**, clipped at tile boundaries. A polygon
  spanning nine tiles comes back as nine rows.
- **Coordinates are quantized** to the tile grid — about 5.4e-6 degrees at
  zoom 14, coarser further out — and lines and polygons are simplified.
- **Features may be missing** wherever `DROP_DENSEST_AS_NEEDED` or a drop rate
  removed them, and below the maximum zoom that is by design.
- **Reads default to the maximum zoom**, the only level where every surviving
  feature is present. Lower zooms hold a thinned, simplified subset.

For round-tripping data faithfully, keep the source. This is for inspecting,
validating and querying what actually ended up in the tiles.

## Notes and limitations

- **Empty results.** tippecanoe exits with an error when it reads no features,
  so `COPY (SELECT ... WHERE false)` fails rather than writing an empty
  tileset. The error text comes straight from tippecanoe.
- **Coordinate precision.** Coordinates are written with 7 decimals by default,
  roughly a centimetre. That is well below a pixel at tippecanoe's maximum
  zoom, and keeping the numbers short measurably reduces the volume pushed
  through the pipe. Raise it with `COORDINATE_PRECISION` if you need more.
- **Empty geometries become a null feature geometry.** `POINT EMPTY` and
  friends have nothing to put in a tile, and tippecanoe rejects an empty Point
  outright — one such row would otherwise fail the whole tileset. A `NULL`
  geometry column is written the same way.
- **The M dimension is dropped**, since GeoJSON has no place for it. Z is kept
  as the third coordinate.
- **SRIDs are ignored.** GeoJSON coordinates are assumed to be WGS84 already;
  reproject in SQL beforehand if they are not.
- **`--no-tile-size-limit` and `--no-feature-limit` are passed by default.**
  Without them tippecanoe aborts the entire tileset as soon as one tile exceeds
  500 KB or 200,000 features, which is easy to hit on a large dense extract.
  Lifting the caps lets those tilesets finish, but individual tiles can then
  grow beyond what some renderers and CDNs will serve — Mapbox GL and several
  hosted tile services reject tiles over roughly 500 KB. If you are producing
  tiles for a renderer with a hard limit, prefer thinning the data with
  `DROP_DENSEST_AS_NEEDED` (or a lower `MAXZOOM`) and restore the caps with
  `NO_TILE_SIZE_LIMIT false, NO_FEATURE_LIMIT false` so oversized tiles are
  reported instead of silently produced.
- **`--read-parallel` is passed by default**, which lets tippecanoe parse the
  incoming features on several threads. It is safe here because every feature
  is written on its own line, which is exactly the input shape that flag
  requires — a pass-through GeoJSON column containing pretty-printed text has
  its line breaks folded to spaces to keep that true. Measured at roughly 20%
  off the total for a million points; turn it off with `READ_PARALLEL false`.
- **Ordering.** Features reach tippecanoe in whatever order the query produces
  them, and tippecanoe re-sorts everything by tile regardless. With
  `SET preserve_insertion_order = false`, serialization runs on several threads.
  If you need `--preserve-input-order` via `ARGS`, pair it with
  `READ_PARALLEL false`, since parallel reading reorders features by design.
- **`SIGPIPE` is set to `SIG_IGN`** process-wide the first time a tileset is
  written, so that tippecanoe exiting early surfaces as a DuckDB error instead
  of killing the DuckDB process.

## Performance

The tippecanoe cloned above ([rjkeller/tippecanoe][fork], branch
`duckdb-tcbf`) is v2.80.0 with performance patches that profiling this
extension motivated: tile writes are batched into SQLite transactions with
cached prepared statements, the largest tiling tasks really are scheduled
first (upstream's largest-first sort compared pointers rather than task
sizes), and the short-lived temporary shard files that carry features
between zoom levels are written uncompressed — compressing them was most of
the single-threaded time at low zoom levels, where few tiles exist and
tippecanoe's per-tile parallelism has nothing to spread across.

[fork]: https://github.com/rjkeller/tippecanoe/tree/duckdb-tcbf

Build it and either put it on your PATH or select it per query with the
`TIPPECANOE` option:

```sh
make -C third_party/tippecanoe -j tippecanoe
```

On 14M parcel polygons at `MAXZOOM 15`, the patches cut the whole `COPY`
from 506s to 225s on an M5 Pro (18 cores); a 1M-polygon benchmark went from
16.2s to 7.9s. The tiles produced are identical.

The uncompressed temporaries occupy roughly 2.4x more transient space in
`TMPDIR` while a build runs. If disk is tighter than CPU, restore the old
behavior with `ARGS ['--compress-temporary-files']`.

### The binary feature protocol

The vendored tippecanoe also accepts TCBF, a binary feature stream this
extension sends in place of GeoJSON text: geometries travel as
delta-encoded web-mercator integers and properties as tagged values, so no
JSON is generated on the DuckDB side or parsed on the tippecanoe side, and
coordinates skip the double → text → double round trip entirely.

Nothing needs configuring. At bind time the extension runs
`tippecanoe --version` once; a version carrying the `-duckdb` suffix gets
the binary stream, anything else gets GeoJSON exactly as before. Queries
that use a pass-through GeoJSON text column, or `KEEP_GEOJSON`, fall back
to GeoJSON automatically, and `BINARY false` forces the fallback.

Measured on the 14M-parcel build, the read phase fell from 55.6s to 8.2s
and system time dropped 6x (the GeoJSON path also spools its entire input
to a temporary file, which the framed binary stream does not need). End to
end that is worth roughly another 1.3x on polygon-heavy builds, more when
attributes are wide. The tiles produced are identical — the wire format
changes, the features do not.

## Tests

```sh
make test
```

The tests in `test/sql/mbtiles.test` use `KEEP_GEOJSON` to assert on the exact
GeoJSON produced for each geometry type, so they cover the conversion without
having to open a tileset.
