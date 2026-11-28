// SPDX-License-Identifier: Apache-2.0
// The process host (geaos_process.h): POSIX (Linux, Android, macOS) and
// Windows. Where a device cannot start processes (microcontrollers, the
// browser, iOS) the host compiles and refuses every launch with a reason.
#include "geaos_process.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(_WIN32)
#define GEAOS_PROCESS_WIN32 1
#define GEAOS_PROCESS_POSIX 0
#elif !defined(ESP_PLATFORM) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#define GEAOS_PROCESS_WIN32 0
#define GEAOS_PROCESS_POSIX 1
#else
#define GEAOS_PROCESS_WIN32 0
#define GEAOS_PROCESS_POSIX 0
#endif

#if GEAOS_PROCESS_POSIX
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char **environ;
#elif GEAOS_PROCESS_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#endif

namespace gea::geaos {

namespace {

using Clock = std::chrono::steady_clock;

#if GEAOS_PROCESS_POSIX
bool setNonBlocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void setCloseOnExec(int fd)
{
	const int flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Writing to an app that just died must not take Home down with SIGPIPE.
void noSigPipe(int fd)
{
#if defined(SO_NOSIGPIPE)
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
	(void)fd;
#endif
}

bool sendAll(int fd, const std::string &data)
{
	std::size_t done = 0;
	while (done < data.size()) {
#if defined(MSG_NOSIGNAL)
		const ssize_t n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
#else
		const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
#endif
		if (n > 0) {
			done += static_cast<std::size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// The app is not reading. A control line is tiny; wait briefly
			// rather than drop it, but never block Home for long.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			static thread_local int spins = 0;
			if (++spins > 200) {
				spins = 0;
				return false;
			}
			continue;
		}
		return false;
	}
	return true;
}
#endif

bool validLine(const std::string &line)
{
	if (line.size() > kMaxIpcLine) return false;
	for (char c : line)
		if (c == '\n' || c == '\0') return false;
	return true;
}

}  // namespace

ProcessHost::ProcessHost(Options options, Fs &fs) : options_(std::move(options)), fs_(fs) {}

ProcessHost::~ProcessHost()
{
	if (pid_ > 0) {
		kill("Home is shutting down");
		waitUntilHome(std::chrono::milliseconds(2000));
	}
	closeChannel();
}

bool ProcessHost::programFor(const InstalledApp &app, std::string &program, std::string &error)
{
	if (app.layout == AppLayout::Folder) {
		program = folderProgramPath(fs_, app.location, options_.targetKey);
		if (program.empty()) {
			error = app.manifest.name + " has no program for " + options_.targetKey;
			return false;
		}
		return true;
	}
	// A package: unpack its program once per version.
	if (options_.cacheDirectory.empty()) {
		error = "this Home has no cache folder to run packaged apps from";
		return false;
	}
	const std::string dir = options_.cacheDirectory + "/" + app.manifest.id + "-" + app.manifest.version.toString();
	program = dir + "/program";
	if (fs_.exists(program)) return true;
	PackageReader reader;
	std::string data;
	if (!reader.open(fs_.open(app.location), error) || !reader.read(programEntryName(options_.targetKey), data, error)) {
		if (error.empty()) error = "cannot read " + app.manifest.name;
		return false;
	}
	fs_.makeDir(options_.cacheDirectory);
	if (!fs_.makeDir(dir)) {
		error = "cannot create " + dir;
		return false;
	}
	const std::string partial = dir + "/.program.partial";
	std::unique_ptr<WritableFile> out = fs_.create(partial);
	if (!out || !out->write(data.data(), data.size()) || !out->finish()) {
		fs_.remove(partial);
		error = "cannot unpack " + app.manifest.name + " to " + dir;
		return false;
	}
#if GEAOS_PROCESS_POSIX
	::chmod(partial.c_str(), 0755);
#endif
	if (!fs_.rename(partial, program)) {
		fs_.remove(partial);
		error = "cannot unpack " + app.manifest.name + " to " + dir;
		return false;
	}
	return true;
}

bool ProcessHost::launch(const InstalledApp &app, std::string &error)
{
	if (!isValidAppId(app.manifest.id)) {
		error = "invalid app id";
		return false;
	}
	std::string program;
	if (!programFor(app, program, error)) return false;
	if (pid_ > 0) {
		// One app at a time: the running one leaves first, whatever it is
		// doing (including still starting).
		stopRunning("another app was launched");
	}
	return spawn(app, program, error);
}

void ProcessHost::startedRunning(const InstalledApp &app, long long pid)
{
	pid_ = pid;
	running_ = app.manifest.id;
	ready_ = false;
	leaving_ = false;
	terminated_ = false;
	killReason_.clear();
	inbox_.clear();
	windowStart_ = Clock::now();
	windowBytes_ = 0;
}

bool ProcessHost::home(std::string &error)
{
	if (pid_ <= 0) {
		error = "no app is running";
		return false;
	}
	if (!leaving_) {
		leaving_ = true;
		backSentAt_ = Clock::now();
		sendRaw("back\n");
	}
	return true;
}

bool ProcessHost::send(const std::string &line)
{
	if (!channelOpen() || !validLine(line)) return false;
	return sendRaw(line + "\n");
}

bool ProcessHost::withinRate(std::size_t bytes)
{
	const auto now = Clock::now();
	if (now - windowStart_ >= std::chrono::seconds(1)) {
		windowStart_ = now;
		windowBytes_ = 0;
	}
	windowBytes_ += bytes;
	return windowBytes_ <= kMaxIpcBytesPerSecond;
}

void ProcessHost::processInbox()
{
	std::size_t start = 0;
	for (std::size_t newline; (newline = inbox_.find('\n', start)) != std::string::npos; start = newline + 1) {
		std::string line = inbox_.substr(start, newline - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line == "ready") {
			ready_ = true;
		} else if (line == "home") {
			// The app is leaving on its own; hold it to the same grace.
			if (!leaving_) {
				leaving_ = true;
				backSentAt_ = Clock::now();
			}
		}
		if (onMessage) onMessage(running_, line);
	}
	inbox_.erase(0, start);
	if (inbox_.size() > kMaxIpcLine) {
		kill("it sent Home a message over 4096 bytes");
		inbox_.clear();
	}
}

void ProcessHost::finishExit(AppExit exit)
{
	pid_ = 0;
	running_.clear();
	ready_ = false;
	leaving_ = false;
	terminated_ = false;
	killReason_.clear();
	closeChannel();
	inbox_.clear();
	lastExit_ = exit;
	if (exit.kind == AppExit::Kind::Crashed) {
		if (exit.signal) {
			Launcher::setLastError(exit.id + " crashed (signal " + std::to_string(exit.signal) + ")");
		} else {
			char code[16];
			std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(exit.code));
			Launcher::setLastError(exit.id + " crashed (" + code + ")");
		}
	} else if (exit.kind == AppExit::Kind::Killed) {
		Launcher::setLastError(exit.id + " was stopped: " + exit.reason);
	}
	if (onExit) onExit(exit);
}

bool ProcessHost::poll()
{
	if (pid_ <= 0) return false;
	const std::string before = running_;
	const bool hadChannel = channelOpen();
	readMessages();
	if (checkExited()) return true;
	const auto now = Clock::now();
	if (leaving_ && !terminated_ && now - backSentAt_ >= options_.backGrace) {
		terminated_ = true;
		terminatedAt_ = now;
		requestTerminate();
	} else if (terminated_ && now - terminatedAt_ >= options_.terminateGrace) {
		kill("it did not leave when asked");
	}
	return running_ != before || (hadChannel && !channelOpen());
}

bool ProcessHost::waitUntilHome(std::chrono::milliseconds timeout)
{
	const auto deadline = Clock::now() + timeout;
	while (pid_ > 0) {
		poll();
		if (pid_ <= 0) break;
		if (Clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

void ProcessHost::stopRunning(const char *reason)
{
	std::string ignored;
	home(ignored);
	// back, then terminate, then kill: bounded by the two grace periods.
	const auto limit = options_.backGrace + options_.terminateGrace + std::chrono::milliseconds(1000);
	if (!waitUntilHome(limit)) {
		kill(reason);
		waitUntilHome(std::chrono::milliseconds(2000));
	}
}

#if GEAOS_PROCESS_POSIX

bool ProcessHost::spawn(const InstalledApp &app, const std::string &program, std::string &error)
{
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		error = std::string("cannot open a channel to the app: ") + std::strerror(errno);
		return false;
	}
	// The exec-failure pipe: the child writes errno here if exec fails; it
	// closes on a successful exec, so EOF means the program is running.
	int report[2];
	if (pipe(report) != 0) {
		::close(pair[0]);
		::close(pair[1]);
		error = std::string("cannot start the app: ") + std::strerror(errno);
		return false;
	}
	// 64 KB each way, like the Windows pipe: room for bursts, and enough
	// that an app flooding the channel reaches the rate limit rather than
	// stalling on a small default buffer.
	for (int fd : pair) {
		int bytes = 64 * 1024;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
	}
	setCloseOnExec(pair[0]);
	setCloseOnExec(report[0]);
	setCloseOnExec(report[1]);
	noSigPipe(pair[0]);

	// Everything the child needs is built before fork: after fork, in a
	// threaded parent, only async-signal-safe calls are allowed.
	// argv: the launcher arguments (if any), then the program; argv[0] runs.
	std::vector<std::string> args = options_.launcherArgs;
	args.push_back(program);
	std::vector<std::string> env;
	for (char **e = environ; e && *e; ++e)
		if (std::strncmp(*e, "GEAOS_", 6) != 0) env.emplace_back(*e);
	env.push_back("GEAOS_IPC_FD=" + std::to_string(pair[1]));
	env.push_back("GEAOS_APP_ID=" + app.manifest.id);
	env.push_back("GEAOS_APP_LOCATION=" + app.location);
	env.push_back("GEAOS_APP_VOLUME=" + app.volumeId);
	for (const std::string &extra : options_.environment) env.push_back(extra);
	std::vector<char *> argv;
	for (std::string &a : args) argv.push_back(a.data());
	argv.push_back(nullptr);
	std::vector<char *> envp;
	for (std::string &e : env) envp.push_back(e.data());
	envp.push_back(nullptr);
	const int childEnd = pair[1];
	const int reportWrite = report[1];

	const pid_t pid = fork();
	if (pid < 0) {
		::close(pair[0]);
		::close(pair[1]);
		::close(report[0]);
		::close(report[1]);
		error = std::string("cannot start the app: ") + std::strerror(errno);
		return false;
	}
	if (pid == 0) {
		// Its own process group, so Home can stop it and anything it started.
		setpgid(0, 0);
#if defined(__linux__)
		// Home gone (crashed, restarted): the app goes with it.
		prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
		signal(SIGPIPE, SIG_DFL);
		execve(argv[0], argv.data(), envp.data());
		const int code = errno;
		(void)!write(reportWrite, &code, sizeof(code));
		_exit(127);
	}
	// Both ends set the group: whichever runs first, killpg finds it.
	setpgid(pid, pid);
	::close(childEnd);
	::close(report[1]);
	int code = 0;
	ssize_t n;
	do {
		n = read(report[0], &code, sizeof(code));
	} while (n < 0 && errno == EINTR);
	::close(report[0]);
	if (n == static_cast<ssize_t>(sizeof(code))) {
		int status = 0;
		waitpid(pid, &status, 0);
		::close(pair[0]);
		error = "cannot run " + app.manifest.name + ": " + std::strerror(code);
		return false;
	}
	setNonBlocking(pair[0]);
	channel_ = pair[0];
	startedRunning(app, pid);
	return true;
}

bool ProcessHost::sendRaw(const std::string &data)
{
	return channel_ >= 0 && sendAll(channel_, data);
}

bool ProcessHost::channelOpen() const { return channel_ >= 0; }

void ProcessHost::kill(const char *reason)
{
	if (pid_ <= 0) return;
	if (killReason_.empty()) killReason_ = reason;
	::killpg(static_cast<pid_t>(pid_), SIGKILL);
	::kill(static_cast<pid_t>(pid_), SIGKILL);
}

void ProcessHost::requestTerminate()
{
	::killpg(static_cast<pid_t>(pid_), SIGTERM);
	::kill(static_cast<pid_t>(pid_), SIGTERM);
}

void ProcessHost::closeChannel()
{
	if (channel_ >= 0) ::close(channel_);
	channel_ = -1;
}

void ProcessHost::readMessages()
{
	if (channel_ < 0) return;
	char buffer[8192];
	for (;;) {
		const ssize_t n = ::recv(channel_, buffer, sizeof(buffer), 0);
		if (n > 0) {
			inbox_.append(buffer, static_cast<std::size_t>(n));
			if (!withinRate(static_cast<std::size_t>(n))) {
				kill("it flooded Home with messages");
				inbox_.clear();
				return;
			}
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		// 0: the app closed its end (exiting). An error other than "nothing
		// waiting" -- ECONNRESET when it exited with our data unread -- is
		// the same end.
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) closeChannel();
		break;
	}
	processInbox();
}

void ProcessHost::reap(int status, const char *killedReason)
{
	AppExit exit;
	exit.id = running_;
	if (WIFEXITED(status)) {
		exit.kind = AppExit::Kind::Exited;
		exit.code = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		exit.signal = WTERMSIG(status);
		if (killedReason && killedReason[0]) {
			exit.kind = AppExit::Kind::Killed;
			exit.reason = killedReason;
		} else if (terminated_) {
			exit.kind = AppExit::Kind::Killed;
			exit.reason = "it did not leave when asked";
		} else {
			exit.kind = AppExit::Kind::Crashed;
		}
	}
	finishExit(exit);
}

bool ProcessHost::checkExited()
{
	int status = 0;
	const pid_t done = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
	if (done == static_cast<pid_t>(pid_)) {
		const std::string reason = killReason_;
		reap(status, reason.c_str());
		return true;
	}
	if (done < 0 && errno == ECHILD) {
		// Reaped elsewhere (a SIGCHLD handler set to ignore): gone.
		reap(0, "");
		return true;
	}
	return false;
}

bool AppChannel::open()
{
	const char *value = std::getenv("GEAOS_IPC_FD");
	if (!value || !*value) return false;
	char *end = nullptr;
	const long fd = std::strtol(value, &end, 10);
	if (!end || *end || fd < 0 || fd > 65535) return false;
	struct stat info {};
	if (fstat(static_cast<int>(fd), &info) != 0 || !S_ISSOCK(info.st_mode)) return false;
	fd_ = static_cast<int>(fd);
	setNonBlocking(fd_);
	setCloseOnExec(fd_);
	noSigPipe(fd_);
	return true;
}

bool AppChannel::send(const std::string &line)
{
	if (fd_ < 0 || !validLine(line)) return false;
	return sendAll(fd_, line + "\n");
}

std::vector<std::string> AppChannel::receive()
{
	std::vector<std::string> out;
	if (fd_ < 0) return out;
	char buffer[4096];
	for (;;) {
		const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
		if (n > 0) {
			inbox_.append(buffer, static_cast<std::size_t>(n));
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		// Closed (0), or reset because Home exited with our data unread.
		const bool ended = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
		if (ended && !sawEnd_) {
			sawEnd_ = true;
			out.push_back("back");  // Home is gone: leave
		}
		break;
	}
	std::size_t start = 0;
	for (std::size_t newline; (newline = inbox_.find('\n', start)) != std::string::npos; start = newline + 1)
		out.push_back(inbox_.substr(start, newline - start));
	inbox_.erase(0, start);
	if (inbox_.size() > kMaxIpcLine) inbox_.clear();
	return out;
}

void AppChannel::close()
{
	if (fd_ >= 0) ::close(fd_);
	fd_ = -1;
}

#elif GEAOS_PROCESS_WIN32

namespace {

std::wstring widen(const std::string &text)
{
	if (text.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring out(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
	return out;
}

std::string systemMessage(DWORD code)
{
	char *text = nullptr;
	const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                               nullptr, code, 0, reinterpret_cast<char *>(&text), 0, nullptr);
	std::string out = n && text ? std::string(text, n) : "error " + std::to_string(code);
	if (text) LocalFree(text);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.' || out.back() == ' ')) out.pop_back();
	return out;
}

// One argument, quoted the way CommandLineToArgvW and the CRT read it back.
void appendArgument(std::wstring &line, const std::wstring &arg)
{
	if (!line.empty()) line += L' ';
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		line += arg;
		return;
	}
	line += L'"';
	for (auto it = arg.begin();; ++it) {
		std::size_t backslashes = 0;
		while (it != arg.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == arg.end()) {
			line.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			line.append(backslashes * 2 + 1, L'\\');
			line += L'"';
		} else {
			line.append(backslashes, L'\\');
			line += *it;
		}
	}
	line += L'"';
}

// Writes everything; Home's end is non-blocking, so a pipe the app does not
// drain gives up after a short wait instead of stalling Home.
bool writeAll(HANDLE pipe, const std::string &data)
{
	std::size_t done = 0;
	int spins = 0;
	while (done < data.size()) {
		DWORD wrote = 0;
		if (!WriteFile(pipe, data.data() + done, static_cast<DWORD>(data.size() - done), &wrote, nullptr)) return false;
		if (wrote == 0) {
			if (++spins > 200) return false;
			Sleep(1);
			continue;
		}
		done += wrote;
	}
	return true;
}

// Reads one chunk of what is waiting, without blocking: >0 bytes read,
// 0 nothing waiting, -1 the other end is gone.
int readSome(HANDLE pipe, char *buffer, DWORD size)
{
	DWORD available = 0;
	if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return -1;
	if (available == 0) return 0;
	DWORD n = 0;
	if (!ReadFile(pipe, buffer, available < size ? available : size, &n, nullptr))
		return GetLastError() == ERROR_NO_DATA ? 0 : -1;
	return static_cast<int>(n);
}

// The exit code TerminateJobObject gives an app Home stopped.
constexpr UINT kStoppedExitCode = 0x4B1;

}  // namespace

bool ProcessHost::spawn(const InstalledApp &app, const std::string &program, std::string &error)
{
	static std::atomic<unsigned> counter{0};
	wchar_t name[96];
	std::swprintf(name, 96, L"\\\\.\\pipe\\geaos-%lu-%u", GetCurrentProcessId(), counter++);
	HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
	                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
	                                 64 * 1024, 64 * 1024, 0, nullptr);
	if (server == INVALID_HANDLE_VALUE) {
		error = "cannot open a channel to the app: " + systemMessage(GetLastError());
		return false;
	}
	SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
	HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, 0, nullptr);
	if (client == INVALID_HANDLE_VALUE || (!ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)) {
		error = "cannot open a channel to the app: " + systemMessage(GetLastError());
		if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
		CloseHandle(server);
		return false;
	}
	DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
	SetNamedPipeHandleState(server, &mode, nullptr, nullptr);

	std::vector<std::string> args = options_.launcherArgs;
	args.push_back(program);
	std::wstring commandLine;
	for (const std::string &a : args) appendArgument(commandLine, widen(a));

	// The environment: Home's, minus any GEAOS_ of its own, plus the app's.
	std::vector<std::wstring> env;
	if (wchar_t *block = GetEnvironmentStringsW()) {
		for (wchar_t *e = block; *e; e += std::wcslen(e) + 1)
			if (_wcsnicmp(e, L"GEAOS_", 6) != 0) env.emplace_back(e);
		FreeEnvironmentStringsW(block);
	}
	env.push_back(L"GEAOS_IPC_FD=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(client)));
	env.push_back(L"GEAOS_APP_ID=" + widen(app.manifest.id));
	env.push_back(L"GEAOS_APP_LOCATION=" + widen(app.location));
	env.push_back(L"GEAOS_APP_VOLUME=" + widen(app.volumeId));
	for (const std::string &extra : options_.environment) env.push_back(widen(extra));
	std::sort(env.begin(), env.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
	std::wstring envBlock;
	for (const std::wstring &e : env) {
		envBlock += e;
		envBlock += L'\0';
	}
	envBlock += L'\0';

	// Only the channel is inherited: not Home's files, sockets or console.
	SIZE_T size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
	std::vector<unsigned char> attributes(size);
	auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	HANDLE inherited[1] = {client};
	const bool listed = InitializeProcThreadAttributeList(list, 1, 0, &size) &&
	                    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.lpAttributeList = listed ? list : nullptr;
	PROCESS_INFORMATION info{};
	// An app's crash comes back to Home as an exit code, not as Windows' crash
	// dialog (the mode is inherited by the app).
	SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	const std::wstring application = widen(args[0]);
	const BOOL started = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
	                                    CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
	                                    envBlock.data(), nullptr, &startup.StartupInfo, &info);
	const DWORD startError = GetLastError();
	if (listed) DeleteProcThreadAttributeList(list);
	CloseHandle(client);
	if (!started) {
		CloseHandle(server);
		error = "cannot run " + app.manifest.name + ": " + systemMessage(startError);
		return false;
	}
	// A job holds the app and everything it starts: stopping the app stops
	// them all, and Home exiting (or crashing) closes the job and ends them.
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		if (!AssignProcessToJobObject(job, info.hProcess)) {
			CloseHandle(job);
			job = nullptr;
		}
	}
	ResumeThread(info.hThread);
	CloseHandle(info.hThread);
	process_ = info.hProcess;
	job_ = job;
	pipe_ = server;
	startedRunning(app, static_cast<long long>(info.dwProcessId));
	return true;
}

bool ProcessHost::sendRaw(const std::string &data) { return pipe_ && writeAll(static_cast<HANDLE>(pipe_), data); }

bool ProcessHost::channelOpen() const { return pipe_ != nullptr; }

void ProcessHost::kill(const char *reason)
{
	if (pid_ <= 0) return;
	if (killReason_.empty()) killReason_ = reason;
	if (job_) TerminateJobObject(static_cast<HANDLE>(job_), kStoppedExitCode);
	if (process_) TerminateProcess(static_cast<HANDLE>(process_), kStoppedExitCode);
}

void ProcessHost::requestTerminate() {}

void ProcessHost::closeChannel()
{
	if (pipe_) CloseHandle(static_cast<HANDLE>(pipe_));
	pipe_ = nullptr;
}

void ProcessHost::readMessages()
{
	if (!pipe_) return;
	char buffer[8192];
	for (;;) {
		const int n = readSome(static_cast<HANDLE>(pipe_), buffer, sizeof(buffer));
		if (n < 0) {
			closeChannel();  // the app closed its end (exiting)
			break;
		}
		if (n == 0) break;
		inbox_.append(buffer, static_cast<std::size_t>(n));
		if (!withinRate(static_cast<std::size_t>(n))) {
			kill("it flooded Home with messages");
			inbox_.clear();
			return;
		}
	}
	processInbox();
}

void ProcessHost::reap(int, const char *) {}

bool ProcessHost::checkExited()
{
	if (!process_ || WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) return false;
	DWORD code = 0;
	GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
	AppExit exit;
	exit.id = running_;
	exit.code = static_cast<int>(code);
	if (!killReason_.empty()) {
		exit.kind = AppExit::Kind::Killed;
		exit.reason = killReason_;
	} else if (code >= 0xC0000000u || code == 0x80000003u) {
		// An NTSTATUS error (access violation, fail-fast, stack overflow) or an
		// unhandled breakpoint: the app crashed.
		exit.kind = AppExit::Kind::Crashed;
	} else {
		exit.kind = AppExit::Kind::Exited;
	}
	CloseHandle(static_cast<HANDLE>(process_));
	process_ = nullptr;
	if (job_) CloseHandle(static_cast<HANDLE>(job_));  // ends anything the app left running
	job_ = nullptr;
	finishExit(exit);
	return true;
}

bool AppChannel::open()
{
	const char *value = std::getenv("GEAOS_IPC_FD");
	if (!value || !*value) return false;
	char *end = nullptr;
	const unsigned long long number = std::strtoull(value, &end, 10);
	if (!end || *end || number == 0) return false;
	HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
	if (GetFileType(handle) != FILE_TYPE_PIPE) return false;
	SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);  // not passed on to what the app starts
	handle_ = handle;
	return true;
}

bool AppChannel::send(const std::string &line)
{
	if (!handle_ || !validLine(line)) return false;
	return writeAll(static_cast<HANDLE>(handle_), line + "\n");
}

std::vector<std::string> AppChannel::receive()
{
	std::vector<std::string> out;
	if (!handle_) return out;
	char buffer[4096];
	for (;;) {
		const int n = readSome(static_cast<HANDLE>(handle_), buffer, sizeof(buffer));
		if (n > 0) {
			inbox_.append(buffer, static_cast<std::size_t>(n));
			continue;
		}
		if (n < 0 && !sawEnd_) {
			sawEnd_ = true;
			out.push_back("back");  // Home is gone: leave
		}
		break;
	}
	std::size_t start = 0;
	for (std::size_t newline; (newline = inbox_.find('\n', start)) != std::string::npos; start = newline + 1)
		out.push_back(inbox_.substr(start, newline - start));
	inbox_.erase(0, start);
	if (inbox_.size() > kMaxIpcLine) inbox_.clear();
	return out;
}

void AppChannel::close()
{
	if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
	handle_ = nullptr;
}


#else

bool ProcessHost::spawn(const InstalledApp &app, const std::string &, std::string &error)
{
	error = "this device cannot run " + app.manifest.name + " as a separate process";
	return false;
}
bool ProcessHost::sendRaw(const std::string &) { return false; }
bool ProcessHost::channelOpen() const { return false; }
void ProcessHost::kill(const char *) {}
void ProcessHost::requestTerminate() {}
void ProcessHost::closeChannel() { channel_ = -1; }
void ProcessHost::readMessages() {}
void ProcessHost::reap(int, const char *) {}
bool ProcessHost::checkExited() { return false; }
bool AppChannel::open() { return false; }
bool AppChannel::send(const std::string &) { return false; }
std::vector<std::string> AppChannel::receive() { return {}; }
void AppChannel::close()
{
	fd_ = -1;
	sawEnd_ = false;
}

#endif

// ---- processPlatform -------------------------------------------------------

namespace processPlatform {

namespace {

// The host a shell installs when it is an app (talks to its Home) or
// standalone (runs only itself).
class SelfHost final : public AppHost {
public:
	SelfHost(std::string id, std::string targetKey, AppChannel *channel)
	    : id_(std::move(id)), targetKey_(std::move(targetKey)), channel_(channel) {}
	const char *targetKey() const override { return targetKey_.c_str(); }
	const char *abi() const override { return kProcessAbi; }
	const char *firmwareId() const override { return id_.c_str(); }
	const char *runningAppId() const override { return channel_ ? id_.c_str() : ""; }
	bool launch(const InstalledApp &app, std::string &error) override
	{
		error = channel_ ? "return to Home to open " + app.manifest.name : "this build runs a single app";
		return false;
	}
	bool home(std::string &error) override
	{
		if (!channel_ || !channel_->send("home")) {
			error = channel_ ? "Home is not answering" : "this build runs a single app";
			return false;
		}
		leaving = true;
		return true;
	}
	bool leaving = false;

private:
	std::string id_;
	std::string targetKey_;
	AppChannel *channel_;
};

struct State {
	Role role = Role::Standalone;
	bool installed = false;
	std::unique_ptr<ProcessHost> host;
	std::unique_ptr<AppChannel> channel;
	std::unique_ptr<SelfHost> self;
	bool readySent = false;
	bool quit = false;
};

State &state()
{
	static State s;
	return s;
}

std::string environment(const char *name)
{
	const char *value = std::getenv(name);
	return value ? value : "";
}

std::string defaultRoot(bool cache)
{
#if defined(_WIN32)
	const std::string base = environment("LOCALAPPDATA");
	return base.empty() ? std::string() : base + (cache ? "/GeaOS/cache" : "/GeaOS");
#else
	const std::string xdg = environment(cache ? "XDG_CACHE_HOME" : "XDG_DATA_HOME");
	if (!xdg.empty()) return xdg + "/geaos";
	const std::string home = environment("HOME");
	return home.empty() ? std::string() : home + (cache ? "/.cache/geaos" : "/.local/share/geaos");
#endif
}

}  // namespace

Role install(const Config &config)
{
	State &s = state();
	if (s.installed) return s.role;
	s.installed = true;
	auto channel = std::make_unique<AppChannel>();
	if (channel->open()) {
		s.role = Role::App;
		s.channel = std::move(channel);
		s.self = std::make_unique<SelfHost>(config.appId, config.targetKey, s.channel.get());
		Launcher::setHost(s.self.get());
		return s.role;
	}
	if (config.appId != "home" && environment("GEAOS_HOME") != "1") {
		s.role = Role::Standalone;
		s.self = std::make_unique<SelfHost>(config.appId, config.targetKey, nullptr);
		Launcher::setHost(s.self.get());
		return s.role;
	}
	s.role = Role::Home;
	std::string apps = !config.appsRoot.empty() ? config.appsRoot : environment("GEAOS_APPS_ROOT");
	if (apps.empty()) apps = defaultRoot(false);
	std::string cache = !config.cacheRoot.empty() ? config.cacheRoot : defaultRoot(true);
	if (cache.empty() && !apps.empty()) cache = apps + "/.cache";
	ProcessHost::Options options;
	options.targetKey = config.targetKey;
	options.abi = kProcessAbi;
	options.firmwareId = config.appId;
	options.cacheDirectory = cache;
	s.host = std::make_unique<ProcessHost>(options);
	Launcher::setHost(s.host.get());
	if (!apps.empty()) {
		Fs &fs = Launcher::fs();
		fs.makeDir(apps);
		fs.makeDir(apps + "/apps");
		recoverInstalls(fs, Volume("local", apps, false, true, "This computer"));
		Launcher::addVolume(Volume("local", apps, false, true, "This computer"));
	}
	Launcher::rescan();
	return s.role;
}

Role role() { return state().role; }

void tick(bool frameShown)
{
	State &s = state();
	if (s.role == Role::Home && s.host) {
		s.host->poll();
		return;
	}
	if (s.role != Role::App || !s.channel) return;
	if (frameShown && !s.readySent) s.readySent = s.channel->send("ready");
	for (const std::string &message : s.channel->receive())
		if (message == "back") s.quit = true;
	if (s.self && s.self->leaving) s.quit = true;
}

bool quitRequested() { return state().quit; }

bool covered()
{
	const State &s = state();
	return s.role == Role::Home && s.host && s.host->pid() > 0;
}

ProcessHost *host() { return state().host.get(); }

void reset()
{
	State &s = state();
	if (Launcher::host() == s.host.get() || Launcher::host() == s.self.get()) Launcher::setHost(nullptr);
	s.host.reset();  // stops a running app
	s.self.reset();
	s.channel.reset();
	s = State{};
}

}  // namespace processPlatform

}  // namespace gea::geaos
