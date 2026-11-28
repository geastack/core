// SPDX-License-Identifier: Apache-2.0
// The process host (geaos_process.h): apps as child processes that start,
// leave when asked, leave on their own, crash, hang, flood Home with
// messages, or lose their Home. The apps are this binary: on POSIX run with
// --child <mode> behind a one-line shell script per app; on Windows a copy of
// it per app that reads its mode from the app's folder. Built by
// run-geaos-process.sh (POSIX) and run-geaos-process.cmd (Windows).
#include "geaos_process.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace gea::geaos;
using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
void setEnv(const char *name, const std::string &value) { _putenv_s(name, value.c_str()); }
void unsetEnv(const char *name) { _putenv_s(name, ""); }
#else
void setEnv(const char *name, const std::string &value) { setenv(name, value.c_str(), 1); }
void unsetEnv(const char *name) { unsetenv(name); }
#endif

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		g_checks++;                                                                    \
		if (!(cond)) {                                                                 \
			g_failures++;                                                              \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                              \
	} while (0)

#define CHECK_CONTAINS(text, needle)                                                                      \
	do {                                                                                                  \
		g_checks++;                                                                                       \
		if (std::string(text).find(needle) == std::string::npos) {                                        \
			g_failures++;                                                                                 \
			std::fprintf(stderr, "%s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, __LINE__,             \
			             std::string(text).c_str(), needle);                                              \
		}                                                                                                 \
	} while (0)

// ---- The apps --------------------------------------------------------------

int runChild(const std::string &mode)
{
	if (mode == "platform-app" || mode == "platform-app-leaves") {
		// A shell's view: the platform finds its Home, reports the first
		// frame and leaves when asked (or asks to leave itself).
		if (processPlatform::install({"plat-app", "test-host", "", ""}) != processPlatform::Role::App) return 94;
		for (int frame = 0; frame < 2000; frame++) {
			processPlatform::tick(true);
			if (processPlatform::quitRequested()) return 0;
			if (mode == "platform-app-leaves" && frame == 5 && !Launcher::home()) return 95;
			std::this_thread::sleep_for(5ms);
		}
		return 96;
	}
	AppChannel channel;
	if (!channel.open()) return 90;  // not started by a host
	const auto waitForBack = [&](int code) {
		for (int i = 0; i < 2000; i++) {
			for (const std::string &message : channel.receive()) {
				if (message == "back") return code;
				if (message.rfind("echo ", 0) == 0) channel.send("got " + message.substr(5));
			}
			std::this_thread::sleep_for(5ms);
		}
		return 91;
	};
	if (mode == "ready-back") {
		channel.send("ready");
		channel.send("log started");
		return waitForBack(0);
	}
	if (mode == "env") {
		const char *id = std::getenv("GEAOS_APP_ID");
		const char *extra = std::getenv("TEST_EXTRA");
		const bool ok = id && std::string(id) == "envapp" && extra && std::string(extra) == "yes" && std::getenv("GEAOS_APP_LOCATION");
		channel.send(ok ? "ready" : "log bad environment");
		return waitForBack(ok ? 0 : 9);
	}
	if (mode == "exit-self") {
		channel.send("ready");
		channel.send("home");
		std::this_thread::sleep_for(20ms);
		return 3;
	}
	if (mode == "crash") {
		channel.send("ready");
		std::this_thread::sleep_for(20ms);
#if defined(_WIN32)
		volatile int *nowhere = nullptr;
		*nowhere = 1;  // an access violation, 0xC0000005
#else
		std::raise(SIGSEGV);
#endif
		return 92;
	}
	if (mode == "crash-on-start") {
#if defined(_WIN32)
		__fastfail(7);  // 0xC0000409
#else
		std::raise(SIGABRT);
#endif
		return 92;
	}
	if (mode == "hang") {
#if !defined(_WIN32)
		std::signal(SIGTERM, SIG_IGN);
#endif
		channel.send("ready");
		for (;;) std::this_thread::sleep_for(1s);
	}
	if (mode == "slow-start") {
		// Never ready: still starting when the next app is launched.
		for (;;) std::this_thread::sleep_for(1s);
	}
	if (mode == "says-home-stays") {
#if !defined(_WIN32)
		std::signal(SIGTERM, SIG_IGN);
#endif
		channel.send("ready");
		channel.send("home");
		for (;;) std::this_thread::sleep_for(1s);
	}
	if (mode == "flood") {
		std::string junk(64 * 1024, 'x');
		junk += '\n';
		for (;;)
			if (!channel.send(junk.substr(0, 4000))) std::this_thread::sleep_for(1ms);
	}
	if (mode == "long-line") {
		const char *fd = std::getenv("GEAOS_IPC_FD");
		const std::string line(6000, 'y');
#if defined(_WIN32)
		DWORD wrote = 0;
		WriteFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(fd, nullptr, 10))), line.data(),
		          static_cast<DWORD>(line.size()), &wrote, nullptr);  // no newline
#else
		(void)!write(std::atoi(fd), line.data(), line.size());  // no newline
#endif
		for (;;) std::this_thread::sleep_for(1s);
	}
	return 93;
}

// ---- Fixtures --------------------------------------------------------------

std::string g_self;
std::string g_root;

void writeFile(const std::string &path, const std::string &text)
{
	std::unique_ptr<WritableFile> out = posixFs().create(path);
	out->write(text.data(), text.size());
	out->finish();
}

[[maybe_unused]] std::string readFile(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

// The program an app runs: on POSIX a script that runs this binary in
// `mode`; on Windows a copy of this binary (it finds `mode` in the app's
// folder). Not executable: a script without the bit, or a text file that is
// no Windows program.
std::string programText(const std::string &mode)
{
#if defined(_WIN32)
	(void)mode;
	return readFile(g_self);
#else
	return "#!/bin/sh\nexec \"" + g_self + "\" --child " + mode + "\n";
#endif
}

std::string writeScript(const std::string &dir, const std::string &mode, bool executable = true)
{
	posixFs().makeDir(dir);
	posixFs().makeDir(dir + "/program");
	const std::string path = dir + "/program/test-host";
#if defined(_WIN32)
	writeFile(dir + "/mode", mode);
	writeFile(path, executable ? programText(mode) : "not a program\n");
#else
	writeFile(path, programText(mode));
	::chmod(path.c_str(), executable ? 0755 : 0644);
#endif
	return path;
}

InstalledApp folderApp(const std::string &id, const std::string &mode, bool executable = true)
{
	InstalledApp app;
	app.manifest.id = id;
	app.manifest.name = id;
	app.manifest.abi = "abi1";
	app.volumeId = "disk";
	app.location = g_root + "/apps/" + id;
	app.layout = AppLayout::Folder;
	writeScript(app.location, mode, executable);
	return app;
}

ProcessHost::Options options()
{
	ProcessHost::Options o;
	o.targetKey = "test-host";
	o.abi = "abi1";
	o.cacheDirectory = g_root + "/cache";
	o.backGrace = 300ms;
	o.terminateGrace = 300ms;
	o.environment = {"TEST_EXTRA=yes"};
	return o;
}

bool waitFor(ProcessHost &host, const std::function<bool()> &done, std::chrono::milliseconds limit = 5s)
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!done()) {
		host.poll();
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::sleep_for(2ms);
	}
	return true;
}

// ---- Tests -------------------------------------------------------------------

void testLifecycle()
{
	ProcessHost host(options());
	std::vector<std::string> messages;
	std::vector<AppExit> exits;
	host.onMessage = [&](const std::string &id, const std::string &m) { messages.push_back(id + ":" + m); };
	host.onExit = [&](const AppExit &e) { exits.push_back(e); };
	std::string error;
	CHECK(!host.home(error));
	CHECK_CONTAINS(error, "no app is running");
	CHECK(std::string(host.runningAppId()).empty());

	CHECK(host.launch(folderApp("alpha", "ready-back"), error));
	CHECK(std::string(host.runningAppId()) == "alpha");
	CHECK(host.pid() > 0);
	CHECK(waitFor(host, [&] { return !host.starting(); }));
	CHECK(waitFor(host, [&] { return messages.size() >= 2; }));
	CHECK(messages.size() >= 2 && messages[0] == "alpha:ready" && messages[1] == "alpha:log started");
	CHECK(host.send("echo hi"));
	CHECK(waitFor(host, [&] { return messages.size() >= 3; }));
	CHECK(messages.size() >= 3 && messages[2] == "alpha:got hi");
	CHECK(!host.send("two\nlines"));
	CHECK(!host.send(std::string(5000, 'z')));
	CHECK(host.home(error));
	CHECK(host.home(error));  // asking twice is harmless
	CHECK(host.waitUntilHome(5s));
	CHECK(exits.size() == 1);
	CHECK(exits[0].id == "alpha" && exits[0].kind == AppExit::Kind::Exited && exits[0].code == 0);
	CHECK(std::string(host.runningAppId()).empty());
	CHECK(host.pid() == 0);
}

void testEnvironmentAndPackages()
{
	ProcessHost host(options());
	std::string error;
	CHECK(host.launch(folderApp("envapp", "env"), error));
	CHECK(waitFor(host, [&] { return !host.starting() || !host.pid(); }));
	host.home(error);
	CHECK(host.waitUntilHome(5s));
	CHECK(host.lastExit().kind == AppExit::Kind::Exited && host.lastExit().code == 0);

	// A package app: its program is unpacked to the cache once and run.
	const std::string script = programText("ready-back");
	PackageWriter writer;
	writer.add("manifest.json", "{\"id\":\"packed\",\"name\":\"Packed\",\"version\":\"2.0.0\",\"abi\":\"abi1\"}");
	writer.add(programEntryName("test-host"), script);
	const std::string bytes = writer.finish();
	posixFs().makeDir(g_root + "/apps");
	std::unique_ptr<WritableFile> out = posixFs().create(g_root + "/apps/packed.geapp");
	out->write(bytes.data(), bytes.size());
	out->finish();
	Volume v("disk", g_root);
	ScanResult scanned = scan({v}, posixFs(), "test-host");
	const InstalledApp *packed = nullptr;
	for (const InstalledApp &app : scanned.apps)
		if (app.manifest.id == "packed") packed = &app;
	CHECK(packed && packed->layout == AppLayout::Package);
	if (packed) {
		CHECK(host.launch(*packed, error));
		CHECK(posixFs().exists(g_root + "/cache/packed-2.0.0/program"));
		CHECK(waitFor(host, [&] { return !host.starting(); }));
		host.home(error);
		CHECK(host.waitUntilHome(5s));
		CHECK(host.lastExit().code == 0);
		// No cache folder: refused with a reason.
		ProcessHost::Options o = options();
		o.cacheDirectory.clear();
		ProcessHost bare(o);
		CHECK(!bare.launch(*packed, error));
		CHECK_CONTAINS(error, "no cache folder");
	}
	// A folder app without a program for this target, and one that cannot
	// be executed.
	InstalledApp none;
	none.manifest.id = "none";
	none.manifest.name = "None";
	none.location = g_root + "/apps/none";
	posixFs().makeDir(none.location);
	CHECK(!host.launch(none, error));
	CHECK_CONTAINS(error, "no program for test-host");
	CHECK(!host.launch(folderApp("noexec", "ready-back", false), error));
	CHECK_CONTAINS(error, "cannot run noexec");
	CHECK(host.pid() == 0);
}

void testLeavingOnItsOwnAndCrashes()
{
	ProcessHost host(options());
	std::string error;
	Launcher::reset();
	CHECK(host.launch(folderApp("selfexit", "exit-self"), error));
	CHECK(host.waitUntilHome(5s));
	CHECK(host.lastExit().kind == AppExit::Kind::Exited && host.lastExit().code == 3);
	CHECK(Launcher::lastError().empty());  // leaving is not an error

	CHECK(host.launch(folderApp("crasher", "crash"), error));
	CHECK(host.waitUntilHome(5s));
#if defined(_WIN32)
	CHECK(host.lastExit().kind == AppExit::Kind::Crashed && static_cast<unsigned>(host.lastExit().code) == 0xC0000005u);
	CHECK_CONTAINS(Launcher::lastError(), "crasher crashed (0xC0000005)");
#else
	CHECK(host.lastExit().kind == AppExit::Kind::Crashed && host.lastExit().signal == SIGSEGV);
	CHECK_CONTAINS(Launcher::lastError(), "crasher crashed (signal 11)");
#endif

	CHECK(host.launch(folderApp("early", "crash-on-start"), error));  // exec succeeded; it dies before ready
	CHECK(host.waitUntilHome(5s));
#if defined(_WIN32)
	CHECK(host.lastExit().kind == AppExit::Kind::Crashed && static_cast<unsigned>(host.lastExit().code) == 0xC0000409u);
#else
	CHECK(host.lastExit().kind == AppExit::Kind::Crashed && host.lastExit().signal == SIGABRT);
#endif
	Launcher::reset();
}

void testHangsAndFloods()
{
	ProcessHost host(options());
	std::string error;
	Launcher::reset();
	// Ignores back (and SIGTERM): killed after both grace periods.
	CHECK(host.launch(folderApp("hanger", "hang"), error));
	CHECK(waitFor(host, [&] { return !host.starting(); }));
	const auto asked = std::chrono::steady_clock::now();
	CHECK(host.home(error));
	CHECK(host.waitUntilHome(5s));
	const auto took = std::chrono::steady_clock::now() - asked;
	CHECK(took >= 550ms && took < 3s);
	CHECK(host.lastExit().kind == AppExit::Kind::Killed);
	CHECK_CONTAINS(host.lastExit().reason, "did not leave");
	CHECK_CONTAINS(Launcher::lastError(), "hanger was stopped");

	// Says it is going home and stays: held to the same grace.
	CHECK(host.launch(folderApp("liar", "says-home-stays"), error));
	CHECK(host.waitUntilHome(5s));
	CHECK(host.lastExit().kind == AppExit::Kind::Killed);

	CHECK(host.launch(folderApp("flooder", "flood"), error));
	CHECK(host.waitUntilHome(5s));
	CHECK(host.lastExit().kind == AppExit::Kind::Killed);
	CHECK_CONTAINS(host.lastExit().reason, "flooded");

	CHECK(host.launch(folderApp("longline", "long-line"), error));
	CHECK(host.waitUntilHome(5s));
	CHECK(host.lastExit().kind == AppExit::Kind::Killed);
	CHECK_CONTAINS(host.lastExit().reason, "over 4096 bytes");
	Launcher::reset();
}

void testOneAtATime()
{
	ProcessHost host(options());
	std::vector<AppExit> exits;
	host.onExit = [&](const AppExit &e) { exits.push_back(e); };
	std::string error;
	CHECK(host.launch(folderApp("first", "ready-back"), error));
	CHECK(waitFor(host, [&] { return !host.starting(); }));
	const long firstPid = host.pid();
	// Launching while another app runs: the first leaves (asked nicely).
	CHECK(host.launch(folderApp("second", "ready-back"), error));
	CHECK(exits.size() == 1 && exits[0].id == "first" && exits[0].kind == AppExit::Kind::Exited);
	CHECK(std::string(host.runningAppId()) == "second");
	CHECK(host.pid() != firstPid);
	// Launching while one is still starting (never ready, ignores back).
	CHECK(host.launch(folderApp("starter", "slow-start"), error));
	CHECK(host.starting());
	CHECK(host.launch(folderApp("third", "ready-back"), error));
	CHECK(std::string(host.runningAppId()) == "third");
	CHECK(exits.size() == 3 && exits[2].id == "starter");
	host.home(error);
	CHECK(host.waitUntilHome(5s));
}

void testThroughTheLauncher()
{
	ProcessHost host(options());
	Launcher::reset();
	Launcher::setHost(&host);
	Launcher::addVolume(Volume("disk", g_root));
	folderApp("viaLauncher", "ready-back");  // invalid id (capital): never listed
	folderApp("via-launcher", "ready-back");
	std::unique_ptr<WritableFile> out = posixFs().create(g_root + "/apps/via-launcher/manifest.json");
	const std::string manifest = "{\"id\":\"via-launcher\",\"name\":\"Via\",\"abi\":\"abi1\"}";
	out->write(manifest.data(), manifest.size());
	out->finish();
	CHECK(Launcher::launch("via-launcher"));
	CHECK(std::string(Launcher::runningAppId()) == "via-launcher");
	CHECK(Launcher::appRunning());
	CHECK(Launcher::home());
	CHECK(host.waitUntilHome(5s));
	CHECK(!Launcher::appRunning());
	CHECK(std::string(Launcher::currentId()) == "home");
	Launcher::reset();
}

// Home restarted or crashed with an app running: the app's end of the
// channel closes and the app reads that as "back".
void testHomeGone()
{
#if defined(_WIN32)
	HANDLE server = CreateNamedPipeW(L"\\\\.\\pipe\\geaos-test-home-gone", PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
	CHECK(server != INVALID_HANDLE_VALUE);
	HANDLE client = CreateFileW(L"\\\\.\\pipe\\geaos-test-home-gone", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	CHECK(client != INVALID_HANDLE_VALUE);
	setEnv("GEAOS_IPC_FD", std::to_string(reinterpret_cast<std::uintptr_t>(client)));
#else
	int pair[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	setEnv("GEAOS_IPC_FD", std::to_string(pair[1]));
#endif
	AppChannel channel;
	CHECK(channel.open());
	CHECK(channel.receive().empty());
	CHECK(channel.send("ready"));
#if defined(_WIN32)
	CloseHandle(server);
#else
	::close(pair[0]);
#endif
	std::vector<std::string> got = channel.receive();
	CHECK(got.size() == 1 && got[0] == "back");
	CHECK(channel.receive().empty());  // said once
	channel.close();
	setEnv("GEAOS_IPC_FD", "not-a-number");
	AppChannel bad;
	CHECK(!bad.open());
#if defined(_WIN32)
	HANDLE file = CreateFileW(L"NUL", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);  // not a pipe
	setEnv("GEAOS_IPC_FD", std::to_string(reinterpret_cast<std::uintptr_t>(file)));
	CHECK(!bad.open());
	CloseHandle(file);
#else
	const int file = ::open("/dev/null", O_RDONLY);  // a descriptor that is not a socket
	setEnv("GEAOS_IPC_FD", std::to_string(file));
	CHECK(!bad.open());
	::close(file);
#endif
	unsetEnv("GEAOS_IPC_FD");
	CHECK(!bad.open());
}

// The host is destroyed with an app running: the app is stopped.
void testShutdown()
{
	long pid = 0;
	{
		ProcessHost host(options());
		std::string error;
		CHECK(host.launch(folderApp("survivor", "hang"), error));
		pid = host.pid();
	}
	CHECK(pid > 0);
#if defined(_WIN32)
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	CHECK(!process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0);  // gone
	if (process) CloseHandle(process);
#else
	CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);  // gone (and reaped)
#endif
}

void testPlatform()
{
	Launcher::reset();
	processPlatform::reset();
	unsetEnv("GEAOS_IPC_FD");
	CHECK(processPlatform::install({"solo", "test-host", "", ""}) == processPlatform::Role::Standalone);
	CHECK(!processPlatform::covered());
	std::string error;
	InstalledApp other;
	other.manifest.name = "Other";
	CHECK(!Launcher::host()->launch(other, error));
	CHECK_CONTAINS(error, "single app");
	processPlatform::reset();
	Launcher::reset();

	// Home: apps from its volume, run as processes, Home covered meanwhile.
	const std::string root = g_root + "/platform";
	CHECK(processPlatform::install({"home", "test-host", root, root + "/cache"}) == processPlatform::Role::Home);
	CHECK(processPlatform::install({"home", "test-host", root, root + "/cache"}) == processPlatform::Role::Home);  // once
	ProcessHost *host = processPlatform::host();
	CHECK(host != nullptr);
	if (!host) return;
	for (const char *mode : {"platform-app", "platform-app-leaves"}) {
		const std::string id = std::string("plat-") + (std::string(mode) == "platform-app" ? "stays" : "leaves");
		const std::string dir = root + "/apps/" + id;
		writeScript(dir, mode);
		writeFile(dir + "/manifest.json", "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"abi\":\"" +
		                                      std::string(processPlatform::kProcessAbi) + "\"}");
	}
	Launcher::rescan();
	CHECK(Launcher::find("plat-stays").has_value());
	CHECK(Launcher::launch("plat-stays"));
	CHECK(processPlatform::covered());
	CHECK(waitFor(*host, [&] { return !host->starting() || !host->pid(); }));
	CHECK(host->pid() > 0);  // ready, not exited
	CHECK(Launcher::home());
	const auto until = std::chrono::steady_clock::now() + 5s;
	while (processPlatform::covered() && std::chrono::steady_clock::now() < until) {
		processPlatform::tick(true);
		std::this_thread::sleep_for(2ms);
	}
	CHECK(!processPlatform::covered());
	CHECK(host->lastExit().kind == AppExit::Kind::Exited && host->lastExit().code == 0);

	// The app asks to go home itself.
	CHECK(Launcher::launch("plat-leaves"));
	CHECK(waitFor(*host, [&] { return !host->pid(); }));
	CHECK(host->lastExit().id == "plat-leaves" && host->lastExit().kind == AppExit::Kind::Exited && host->lastExit().code == 0);
	processPlatform::reset();
	Launcher::reset();
}

// Home killed outright (a crash, an OOM kill) with an app running: the app
// must not outlive it. Linux (parent-death signal) and Windows (the job
// closes with Home) stop even an app that ignores everything; elsewhere the
// app leaves because its channel closed.
#if defined(__linux__) || defined(_WIN32)
constexpr const char *kOrphanMode = "hang";
#else
constexpr const char *kOrphanMode = "ready-back";
#endif

int runHome(const std::string &root)
{
	ProcessHost host(options());
	std::string error;
	if (!host.launch(folderApp("orphan", kOrphanMode), error)) return 80;
	writeFile(root + "/orphan.pid", std::to_string(host.pid()));
	for (;;) {
		host.poll();
		std::this_thread::sleep_for(10ms);
	}
}

bool processAlive(long pid)
{
#if defined(_WIN32)
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (!process) return false;
	const bool alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
	CloseHandle(process);
	return alive;
#else
	return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

void testHomeKilled()
{
	const std::string pidFile = g_root + "/orphan.pid";
	posixFs().remove(pidFile);
#if defined(_WIN32)
	std::string line = "\"" + g_self + "\" --home \"" + g_root + "\"";
	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION home{};
	CHECK(CreateProcessA(g_self.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &home));
#else
	const pid_t home = fork();
	if (home == 0) {
		execl(g_self.c_str(), g_self.c_str(), "--home", g_root.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	CHECK(home > 0);
#endif
	long orphan = 0;
	for (int i = 0; i < 500 && !orphan; i++) {
		orphan = std::atol(readFile(pidFile).c_str());
		if (!orphan) std::this_thread::sleep_for(10ms);
	}
	CHECK(orphan > 0);
	CHECK(orphan > 0 && processAlive(orphan));
	std::this_thread::sleep_for(100ms);  // the app is up and reading its channel
#if defined(_WIN32)
	TerminateProcess(home.hProcess, 9);
	WaitForSingleObject(home.hProcess, 5000);
	CloseHandle(home.hProcess);
	CloseHandle(home.hThread);
#else
	::kill(home, SIGKILL);
	int status = 0;
	waitpid(home, &status, 0);
#endif
	bool gone = false;
	for (int i = 0; i < 300 && orphan > 0 && !gone; i++) {
		gone = !processAlive(orphan);
		if (!gone) std::this_thread::sleep_for(10ms);
	}
	CHECK(gone);
#if !defined(_WIN32)
	if (!gone && orphan > 0) ::kill(static_cast<pid_t>(orphan), SIGKILL);  // don't leave it behind
#endif
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc >= 3 && std::string(argv[1]) == "--child") return runChild(argv[2]);
	if (argc >= 3 && std::string(argv[1]) == "--home") {
		g_root = argv[2];
#if defined(_WIN32)
		char self[MAX_PATH];
		g_self = GetModuleFileNameA(nullptr, self, MAX_PATH) ? self : argv[0];
#else
		g_self = argv[0];
#endif
		return runHome(g_root);
	}
#if defined(_WIN32)
	if (std::getenv("GEAOS_IPC_FD")) {
		// An app copy: its mode is in its folder (a package app has none).
		const char *location = std::getenv("GEAOS_APP_LOCATION");
		const std::string mode = location ? readFile(std::string(location) + "/mode") : "";
		return runChild(mode.empty() ? "ready-back" : mode);
	}
	char resolved[MAX_PATH];
	g_self = GetModuleFileNameA(nullptr, resolved, MAX_PATH) ? resolved : argv[0];
	const char *tmp = std::getenv("TMPDIR");
	if (!tmp) tmp = std::getenv("TEMP");
#else
	char resolved[4096];
	g_self = realpath(argv[0], resolved) ? resolved : argv[0];
	const char *tmp = std::getenv("TMPDIR");
#endif
	g_root = std::string(tmp ? tmp : ".") + "/geaos-process-test";
	posixFs().removeTree(g_root);
	posixFs().makeDir(g_root);
	posixFs().makeDir(g_root + "/apps");
	testLifecycle();
	testEnvironmentAndPackages();
	testLeavingOnItsOwnAndCrashes();
	testHangsAndFloods();
	testOneAtATime();
	testThroughTheLauncher();
	testHomeGone();
	testShutdown();
	testPlatform();
	testHomeKilled();
	posixFs().removeTree(g_root);
	if (g_failures) {
		std::fprintf(stderr, "FAIL geaos process host: %d of %d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("PASS geaos process host: %d checks\n", g_checks);
	return 0;
}
