#!/usr/bin/env bash
#
# Start a DuckDB shell with the tippecanoe extension ready to use.
#
#   ./start.sh                          interactive shell
#   ./start.sh mydata.duckdb            open a database
#   ./start.sh -c "COPY (...) TO ..."   run one statement and exit
#
# Any arguments are passed through to duckdb.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIPPECANOE_DIR="$ROOT/third_party/tippecanoe"
TIPPECANOE_BIN="$TIPPECANOE_DIR/tippecanoe"
EXTENSION="$ROOT/build/release/extension/tippecanoe/tippecanoe.duckdb_extension"
BUNDLED_DUCKDB="$ROOT/build/release/duckdb"

note() { [[ -t 2 ]] && printf '%s\n' "$*" >&2; return 0; }
die()  { printf 'start.sh: %s\n' "$*" >&2; exit 1; }

# The vendored tippecanoe carries the performance patches and speaks the binary
# feature protocol. Build it on first run.
if [[ ! -x "$TIPPECANOE_BIN" ]]; then
	[[ -f "$TIPPECANOE_DIR/Makefile" ]] || die "no vendored tippecanoe at $TIPPECANOE_DIR"
	note "Building vendored tippecanoe (first run only)..."
	make -C "$TIPPECANOE_DIR" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" tippecanoe >&2
fi

# Ahead of Homebrew and /usr/local, both of which may hold a stock tippecanoe.
export PATH="$TIPPECANOE_DIR:$PATH"

# Only a version carrying the -duckdb suffix gets the binary feature protocol;
# anything else silently falls back to streaming GeoJSON text.
version="$(tippecanoe --version 2>&1 | head -1 || true)"
case "$version" in
	*-duckdb*) ;;
	*) note "warning: 'tippecanoe' resolves to $(command -v tippecanoe) ($version)"
	   note "         without the -duckdb suffix, the binary feature protocol is off" ;;
esac

# The build/release binary has the extension statically linked; a stock duckdb
# has to load it unsigned.
if [[ -x "$BUNDLED_DUCKDB" ]]; then
	note "duckdb     $BUNDLED_DUCKDB (extension linked in)"
	note "tippecanoe $TIPPECANOE_BIN ($version)"
	note ""
	exec "$BUNDLED_DUCKDB" "$@"
elif command -v duckdb >/dev/null 2>&1; then
	[[ -f "$EXTENSION" ]] || die "no extension built at $EXTENSION -- run 'make release' first"
	note "duckdb     $(command -v duckdb) (loading $EXTENSION unsigned)"
	note "tippecanoe $TIPPECANOE_BIN ($version)"
	note ""
	exec duckdb -unsigned -cmd "LOAD '${EXTENSION//\'/\'\'}';" "$@"
else
	die "no duckdb found -- run 'make release', or install duckdb"
fi
