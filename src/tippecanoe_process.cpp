#include "tippecanoe_process.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
// A shared library on macOS cannot reference `environ` directly.
#include <crt_externs.h>
#define TIPPECANOE_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define TIPPECANOE_ENVIRON environ
#endif
#endif

namespace duckdb {

//! How much of tippecanoe's raw output to consider when building an error
//! message. Most of it is progress updates that get filtered back out.
static constexpr idx_t MAX_DIAGNOSTICS_BYTES = 65536;

//! How many meaningful lines to quote from each end of that output. Both ends
//! matter: a rejected argument makes tippecanoe print the complaint first and
//! then its whole usage text, while a tiling failure comes last of all.
static constexpr idx_t MAX_DIAGNOSTICS_HEAD_LINES = 4;
static constexpr idx_t MAX_DIAGNOSTICS_TAIL_LINES = 16;

//! tippecanoe reports progress by rewriting one line with carriage returns, so
//! a raw capture is almost entirely percentages with the real error buried at
//! the end. Split on both terminators and drop the progress updates.
static string SummarizeDiagnostics(const string &raw) {
	// tippecanoe has several progress formats ("91.7%  13/4118/4050",
	// "Reordering geometry: 84%", ...) and they all carry a percent sign, while
	// its actual diagnostics do not. That one character separates them far more
	// reliably than trying to match each layout.
	auto is_progress = [](const string &line) {
		return line.find('%') != string::npos;
	};

	vector<string> lines;
	string current;
	auto finish_line = [&]() {
		StringUtil::Trim(current);
		if (!current.empty() && !is_progress(current)) {
			lines.push_back(current);
		}
		current.clear();
	};
	for (auto c : raw) {
		if (c == '\n' || c == '\r') {
			finish_line();
		} else {
			current += c;
		}
	}
	finish_line();

	string result;
	auto append = [&result](const string &line) {
		if (!result.empty()) {
			result += '\n';
		}
		result += line;
	};

	if (lines.size() <= MAX_DIAGNOSTICS_HEAD_LINES + MAX_DIAGNOSTICS_TAIL_LINES) {
		for (auto &line : lines) {
			append(line);
		}
		return result;
	}
	for (idx_t i = 0; i < MAX_DIAGNOSTICS_HEAD_LINES; i++) {
		append(lines[i]);
	}
	append(StringUtil::Format("... (%llu more lines)",
	                          static_cast<unsigned long long>(lines.size() - MAX_DIAGNOSTICS_HEAD_LINES -
	                                                          MAX_DIAGNOSTICS_TAIL_LINES)));
	for (idx_t i = lines.size() - MAX_DIAGNOSTICS_TAIL_LINES; i < lines.size(); i++) {
		append(lines[i]);
	}
	return result;
}

static string QuoteArgument(const string &argument) {
	// Only for display; the real argv never goes through a shell.
	bool needs_quotes = argument.empty();
	for (auto c : argument) {
		if (!StringUtil::CharacterIsAlphaNumeric(c) && c != '-' && c != '_' && c != '.' && c != '/' && c != '=' &&
		    c != ':' && c != ',') {
			needs_quotes = true;
			break;
		}
	}
	if (!needs_quotes) {
		return argument;
	}
	return "'" + StringUtil::Replace(argument, "'", "'\\''") + "'";
}

static string BuildCommandLine(const string &executable, const vector<string> &arguments) {
	string result = QuoteArgument(executable);
	for (auto &argument : arguments) {
		result += " ";
		result += QuoteArgument(argument);
	}
	return result;
}

#ifdef _WIN32

bool TippecanoeSupportsTCBF(const string &) {
	return false;
}

TippecanoeProcess::TippecanoeProcess(const string &executable, const vector<string> &arguments) {
	command_line = BuildCommandLine(executable, arguments);
	throw NotImplementedException("COPY ... TO ... (FORMAT mbtiles) is not supported on Windows, because it relies on "
	                              "spawning the tippecanoe binary");
}

TippecanoeProcess::~TippecanoeProcess() {
}
void TippecanoeProcess::Write(const char *, idx_t) {
}
void TippecanoeProcess::Finish() {
}
string TippecanoeProcess::ReadDiagnostics() {
	return string();
}
void TippecanoeProcess::Reap() {
}
void TippecanoeProcess::CloseInput() {
}

#else

//! tippecanoe dying early would otherwise take the whole DuckDB process down
//! with a SIGPIPE. Ignoring it turns that into an EPIPE we can report properly.
static void IgnoreSigPipeOnce() {
	static std::once_flag flag;
	std::call_once(flag, []() { signal(SIGPIPE, SIG_IGN); });
}

//! Creates the unlinked temporary file that captures the child's output.
static int CreateDiagnosticsFile() {
	const char *tmp_dir = getenv("TMPDIR");
	string path = tmp_dir && *tmp_dir ? string(tmp_dir) : string("/tmp");
	if (path.back() != '/') {
		path += '/';
	}
	path += "duckdb_tippecanoe_XXXXXX";

	vector<char> tmp_template(path.begin(), path.end());
	tmp_template.push_back('\0');
	const int fd = mkstemp(tmp_template.data());
	if (fd < 0) {
		throw IOException("Failed to create a temporary file for tippecanoe output: %s", strerror(errno));
	}
	// The file stays reachable through the descriptors we and the child hold,
	// and disappears on its own once they are all closed.
	unlink(tmp_template.data());
	return fd;
}

static void SetCloseOnExec(int fd) {
	const int flags = fcntl(fd, F_GETFD);
	if (flags >= 0) {
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	}
}

TippecanoeProcess::TippecanoeProcess(const string &executable, const vector<string> &arguments, bool capture_output) {
	command_line = BuildCommandLine(executable, arguments);
	IgnoreSigPipeOnce();

	if (capture_output) {
		diagnostics_fd = CreateDiagnosticsFile();
	}

	int pipe_fds[2];
	if (pipe(pipe_fds) != 0) {
		const auto error = string(strerror(errno));
		if (diagnostics_fd >= 0) {
			close(diagnostics_fd);
			diagnostics_fd = -1;
		}
		throw IOException("Failed to create a pipe for tippecanoe: %s", error);
	}
	// Keep our ends out of any other child this process may spawn concurrently.
	SetCloseOnExec(pipe_fds[1]);
	if (diagnostics_fd >= 0) {
		SetCloseOnExec(diagnostics_fd);
	}

	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
	posix_spawn_file_actions_adddup2(&file_actions, pipe_fds[0], STDIN_FILENO);
	if (diagnostics_fd >= 0) {
		posix_spawn_file_actions_adddup2(&file_actions, diagnostics_fd, STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&file_actions, diagnostics_fd, STDERR_FILENO);
	}
	// Without a diagnostics file the child simply inherits our stdout and
	// stderr, so its progress meter reaches the terminal live.
	posix_spawn_file_actions_addclose(&file_actions, pipe_fds[0]);

	vector<char *> argv;
	argv.push_back(const_cast<char *>(executable.c_str()));
	for (auto &argument : arguments) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);

	pid_t child_pid = -1;
	// posix_spawn rather than fork/exec: DuckDB is heavily multi-threaded, and
	// almost nothing is legal to call between fork() and exec() in that setting.
	const int spawn_result =
	    posix_spawnp(&child_pid, executable.c_str(), &file_actions, nullptr, argv.data(), TIPPECANOE_ENVIRON);

	posix_spawn_file_actions_destroy(&file_actions);
	close(pipe_fds[0]);

	if (spawn_result != 0) {
		const auto error = string(strerror(spawn_result));
		close(pipe_fds[1]);
		if (diagnostics_fd >= 0) {
			close(diagnostics_fd);
			diagnostics_fd = -1;
		}
		throw IOException("Failed to start tippecanoe (%s): %s. Is tippecanoe installed and on your PATH? A specific "
		                  "binary can be selected with the TIPPECANOE option.",
		                  command_line, error);
	}

	input_fd = pipe_fds[1];
	pid = child_pid;
}

TippecanoeProcess::~TippecanoeProcess() {
	lock_guard<mutex> guard(lock);
	if (finished) {
		return;
	}
	// The query was aborted. Closing stdin here would let tippecanoe treat the
	// truncated input as complete and happily write a partial tileset, so kill
	// it outright instead.
	CloseInput();
	if (pid >= 0) {
		kill(static_cast<pid_t>(pid), SIGKILL);
		Reap();
	}
	if (diagnostics_fd >= 0) {
		close(diagnostics_fd);
		diagnostics_fd = -1;
	}
}

void TippecanoeProcess::CloseInput() {
	if (input_fd >= 0) {
		close(input_fd);
		input_fd = -1;
	}
}

void TippecanoeProcess::Reap() {
	if (pid < 0) {
		return;
	}
	int status;
	while (waitpid(static_cast<pid_t>(pid), &status, 0) < 0) {
		if (errno != EINTR) {
			break;
		}
	}
	pid = -1;
}

string TippecanoeProcess::ReadDiagnostics() {
	if (diagnostics_fd < 0) {
		return string();
	}
	struct stat file_info;
	if (fstat(diagnostics_fd, &file_info) != 0 || file_info.st_size <= 0) {
		return string();
	}
	const auto size = static_cast<idx_t>(file_info.st_size);
	const auto to_read = MinValue<idx_t>(size, MAX_DIAGNOSTICS_BYTES);
	if (lseek(diagnostics_fd, static_cast<off_t>(size - to_read), SEEK_SET) < 0) {
		return string();
	}

	string result;
	result.resize(to_read);
	idx_t offset = 0;
	while (offset < to_read) {
		const auto bytes = read(diagnostics_fd, &result[offset], to_read - offset);
		if (bytes < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (bytes == 0) {
			break;
		}
		offset += static_cast<idx_t>(bytes);
	}
	result.resize(offset);
	return SummarizeDiagnostics(result);
}

void TippecanoeProcess::Write(const char *data, idx_t size) {
	lock_guard<mutex> guard(lock);
	if (input_fd < 0) {
		throw InternalException("Attempted to write to a tippecanoe process that is no longer accepting input");
	}
	idx_t offset = 0;
	while (offset < size) {
		const auto written = write(input_fd, data + offset, size - offset);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			// EPIPE means tippecanoe exited early - almost always because it
			// rejected the input - so its own message is the useful one.
			const auto error = string(strerror(errno));
			CloseInput();
			Reap();
			const auto diagnostics = ReadDiagnostics();
			throw IOException("tippecanoe exited before all features were written (%s)%s%s", error,
			                  diagnostics.empty() ? "" : "\ntippecanoe output:\n", diagnostics);
		}
		offset += static_cast<idx_t>(written);
	}
}

void TippecanoeProcess::Finish() {
	lock_guard<mutex> guard(lock);
	if (finished) {
		return;
	}
	finished = true;

	// Closing stdin is what tells tippecanoe the feature stream is complete.
	CloseInput();

	int status = 0;
	bool have_status = false;
	if (pid >= 0) {
		while (true) {
			if (waitpid(static_cast<pid_t>(pid), &status, 0) >= 0) {
				have_status = true;
				break;
			}
			if (errno != EINTR) {
				break;
			}
		}
		pid = -1;
	}

	const auto diagnostics = ReadDiagnostics();
	if (diagnostics_fd >= 0) {
		close(diagnostics_fd);
		diagnostics_fd = -1;
	}

	if (!have_status) {
		throw IOException("Failed to wait for tippecanoe (%s): %s", command_line, strerror(errno));
	}
	if (WIFSIGNALED(status)) {
		throw IOException("tippecanoe was terminated by signal %d (%s)%s%s", WTERMSIG(status), command_line,
		                  diagnostics.empty() ? "" : "\ntippecanoe output:\n", diagnostics);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		throw IOException("tippecanoe failed with exit code %d (%s)%s%s", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
		                  command_line, diagnostics.empty() ? "" : "\ntippecanoe output:\n", diagnostics);
	}
}

//! Runs `executable --version` and captures its combined output. Any failure
//! along the way just yields an empty string.
static string ReadTippecanoeVersion(const string &executable) {
	IgnoreSigPipeOnce();

	int output_pipe[2];
	if (pipe(output_pipe) != 0) {
		return string();
	}
	SetCloseOnExec(output_pipe[0]);

	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
	posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);

	const char *version_flag = "--version";
	char *argv[] = {const_cast<char *>(executable.c_str()), const_cast<char *>(version_flag), nullptr};

	pid_t child_pid = -1;
	const int spawn_result =
	    posix_spawnp(&child_pid, executable.c_str(), &file_actions, nullptr, argv, TIPPECANOE_ENVIRON);
	posix_spawn_file_actions_destroy(&file_actions);
	close(output_pipe[1]);

	if (spawn_result != 0) {
		close(output_pipe[0]);
		return string();
	}

	string output;
	char buffer[256];
	while (true) {
		const auto bytes = read(output_pipe[0], buffer, sizeof(buffer));
		if (bytes < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (bytes == 0) {
			break;
		}
		output.append(buffer, static_cast<idx_t>(bytes));
		// --version prints one short line; anything chatty is not tippecanoe.
		if (output.size() > 4096) {
			break;
		}
	}
	close(output_pipe[0]);

	int status;
	while (waitpid(child_pid, &status, 0) < 0) {
		if (errno != EINTR) {
			break;
		}
	}
	return output;
}

bool TippecanoeSupportsTCBF(const string &executable) {
	static mutex cache_lock;
	static unordered_map<string, bool> cache;

	lock_guard<mutex> guard(cache_lock);
	auto entry = cache.find(executable);
	if (entry != cache.end()) {
		return entry->second;
	}
	const auto version = ReadTippecanoeVersion(executable);
	const bool supported = version.find("-duckdb") != string::npos;
	cache[executable] = supported;
	return supported;
}

#endif

} // namespace duckdb
