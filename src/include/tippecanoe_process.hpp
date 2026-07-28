//===----------------------------------------------------------------------===//
//                         DuckDB - tippecanoe extension
//
// tippecanoe_process.hpp
//
// A running tippecanoe child process whose standard input we own.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! Spawns tippecanoe and streams GeoJSON into its standard input.
//!
//! The child's stdout and stderr are captured to an unlinked temporary file so
//! that its diagnostics can be folded into the DuckDB error when it fails.
//! All public methods are safe to call from several threads at once.
class TippecanoeProcess {
public:
	//! Starts `executable` (looked up on PATH when it is not a path) with
	//! `arguments` as argv[1..]. Throws IOException if the child cannot start.
	//!
	//! When `capture_output` is set, the child's stdout and stderr are captured
	//! so they can be folded into the error message if it fails. Clearing it
	//! lets the child write straight to the terminal instead, which is what
	//! makes tippecanoe's progress meter visible on a long build - at the cost
	//! of no longer being able to quote its output back in an exception.
	TippecanoeProcess(const string &executable, const vector<string> &arguments, bool capture_output = true);
	//! Kills the child if Finish() was never reached, so that an aborted query
	//! cannot leave tippecanoe running on - and completing - a partial tileset.
	~TippecanoeProcess();

	TippecanoeProcess(const TippecanoeProcess &) = delete;
	TippecanoeProcess &operator=(const TippecanoeProcess &) = delete;

	//! Writes the entire buffer to the child's standard input.
	void Write(const char *data, idx_t size);
	//! Closes standard input, waits for the child, and throws if it failed.
	//! Subsequent calls do nothing.
	void Finish();

	//! A shell-style rendering of the command, for error messages.
	const string &CommandLine() const {
		return command_line;
	}

private:
	//! Reads back whatever the child wrote to stdout/stderr, tail-truncated.
	string ReadDiagnostics();
	//! Reaps the child, ignoring its status. Caller must hold `lock`.
	void Reap();
	void CloseInput();

	mutex lock;
	string command_line;
	int input_fd = -1;
	int diagnostics_fd = -1;
	int64_t pid = -1;
	bool finished = false;
};

//! Whether `executable` is a tippecanoe build that accepts the TCBF binary
//! feature protocol, detected by the "-duckdb" suffix its --version output
//! carries. Results are cached per executable path; failures of any kind
//! simply report false, which selects the GeoJSON path.
bool TippecanoeSupportsTCBF(const string &executable);

} // namespace duckdb
