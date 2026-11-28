// SPDX-License-Identifier: Apache-2.0
// The process host: every app is its own process, and Home supervises them.
// Linux, GeaOS Linux devices, Android and desktop development use it; a crash
// or a leak stays inside the app's process.
//
// Home and the app talk over one byte stream -- a socketpair on POSIX, a
// named pipe on Windows; the app finds its end (a descriptor or a handle
// value) in GEAOS_IPC_FD -- one message per line:
//
//   Home -> app   back                 leave: save and exit
//                 focus 0|1            the app lost or regained the screen
//                 setting <key> <val>  a system setting changed
//   app -> Home   ready                the first frame is up
//                 home                 the app asks to return to Home
//                 log <text>           a line for Home's log
//
// A line is at most kMaxIpcLine bytes. An app that sends more than
// kMaxIpcBytesPerSecond, or an over-long line, is killed: Home must stay
// responsive whatever an app does.
#pragma once

#include "geaos.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gea::geaos {

constexpr std::size_t kMaxIpcLine = 4096;
constexpr std::size_t kMaxIpcBytesPerSecond = 1024 * 1024;

// How an app's process ended.
struct AppExit {
	std::string id;
	enum class Kind : std::uint8_t { Exited, Crashed, Killed } kind = Kind::Exited;
	int code = 0;    // exit status for Exited
	int signal = 0;  // the signal for Crashed / Killed (POSIX)
	std::string reason;  // for Killed: why Home killed it
};

class ProcessHost : public AppHost {
public:
	struct Options {
		std::string targetKey;  // "linux-x64", "android-arm64", "macos-arm64" ...
		std::string abi;
		std::string firmwareId = "home";
		// Where a package app's program is unpacked to run (it must be a file
		// to exec). Required for package apps.
		std::string cacheDirectory;
		// How long an app has to leave after "back" before it is terminated,
		// and after that before it is killed.
		std::chrono::milliseconds backGrace{2000};
		std::chrono::milliseconds terminateGrace{2000};
		// Extra environment for every app ("KEY=value").
		std::vector<std::string> environment;
		// Arguments put before the program (a test runs itself as the app).
		std::vector<std::string> launcherArgs;
	};

	explicit ProcessHost(Options options, Fs &fs = posixFs());
	~ProcessHost() override;
	ProcessHost(const ProcessHost &) = delete;
	ProcessHost &operator=(const ProcessHost &) = delete;

	const char *targetKey() const override { return options_.targetKey.c_str(); }
	const char *abi() const override { return options_.abi.c_str(); }
	const char *firmwareId() const override { return options_.firmwareId.c_str(); }
	const char *runningAppId() const override { return running_.c_str(); }

	// Starts the app's process. An app already running is asked to leave
	// first (back, then terminate, then kill) -- one app at a time.
	bool launch(const InstalledApp &app, std::string &error) override;
	// Asks the running app to leave; poll() reports when it has.
	bool home(std::string &error) override;

	// The supervisor's tick: reads the app's messages, notices it exited,
	// enforces the grace periods and the IPC limits. Returns true if
	// anything changed (an exit, a message).
	bool poll();
	// Blocks until the app is gone (or `timeout`), polling. For shutdown and
	// tests.
	bool waitUntilHome(std::chrono::milliseconds timeout);

	// Sends a line to the running app ("focus 0", "setting brightness 40").
	bool send(const std::string &line);

	// Called from poll(): every message from the app, and every exit.
	std::function<void(const std::string &id, const std::string &message)> onMessage;
	std::function<void(const AppExit &exit)> onExit;

	// The last exit, for Home's notice ("Ember crashed").
	const AppExit &lastExit() const { return lastExit_; }
	// True between launch() and the app's first "ready".
	bool starting() const { return !running_.empty() && !ready_; }
	// The process id of the running app (0 if none).
	long pid() const { return static_cast<long>(pid_); }

private:
	bool spawn(const InstalledApp &app, const std::string &program, std::string &error);
	bool programFor(const InstalledApp &app, std::string &program, std::string &error);
	void stopRunning(const char *reason);
	void reap(int status, const char *killedReason);
	void finishExit(AppExit exit);
	void readMessages();
	// Splits what readMessages() received into lines and acts on them.
	void processInbox();
	// Counts received bytes against the rate limit; false when over it.
	bool withinRate(std::size_t bytes);
	void startedRunning(const InstalledApp &app, long long pid);
	void kill(const char *reason);
	// The second stage of leaving: SIGTERM on POSIX. Windows has no polite
	// signal for a GUI process, so there the stage only waits out
	// terminateGrace before the kill.
	void requestTerminate();
	bool checkExited();
	bool sendRaw(const std::string &data);
	void closeChannel();
	bool channelOpen() const;

	Options options_;
	Fs &fs_;
	std::string running_;
	bool ready_ = false;
	long long pid_ = 0;
#if defined(_WIN32)
	// The app's process, the job that holds it (and anything it starts), and
	// Home's end of the named pipe.
	void *process_ = nullptr;
	void *job_ = nullptr;
	void *pipe_ = nullptr;
#else
	int channel_ = -1;
#endif
	std::string inbox_;
	AppExit lastExit_;
	// Leaving: when "back" was sent, and when SIGTERM was.
	std::chrono::steady_clock::time_point backSentAt_{};
	std::chrono::steady_clock::time_point terminatedAt_{};
	bool leaving_ = false;
	bool terminated_ = false;
	std::string killReason_;
	// The IPC rate limit's window.
	std::chrono::steady_clock::time_point windowStart_{};
	std::size_t windowBytes_ = 0;
};

// The app's side: its end of the channel, found in GEAOS_IPC_FD.
class AppChannel {
public:
	// Opens the channel Home passed; false when the app was not started by a
	// process host (run on its own, it has no Home to talk to).
	bool open();
#if defined(_WIN32)
	bool connected() const { return handle_ != nullptr; }
#else
	bool connected() const { return fd_ >= 0; }
#endif
	bool send(const std::string &line);
	// Reads what Home sent since the last call, one message per entry. A
	// closed channel (Home exited or was killed) reports "back": an app
	// without its Home leaves.
	std::vector<std::string> receive();
	void close();
	~AppChannel() { close(); }

private:
#if defined(_WIN32)
	void *handle_ = nullptr;  // the inherited pipe end
#else
	int fd_ = -1;
#endif
	std::string inbox_;
	bool sawEnd_ = false;
};

// The process model wired into a shell (a Linux, Windows or GeaOS device
// target): call install() once at start, tick() every frame.
//
// A shell is one of three things, decided at install():
//   App        a Home started it (GEAOS_IPC_FD is set): it tells Home when
//              its first frame is up and leaves when Home says "back";
//              Launcher::home() asks Home to take the screen back.
//   Home       its id is "home" (or GEAOS_HOME=1): it runs installed apps as
//              child processes from the apps volume -- GEAOS_APPS_ROOT, else
//              the per-user data folder -- and steps aside while one runs.
//   Standalone anything else: one app, and launching another is refused.
namespace processPlatform {

enum class Role : std::uint8_t { Standalone, Home, App };

struct Config {
	std::string appId;      // this shell's own app id
	std::string targetKey;  // "linux-x64", "linux-arm64", "windows-x64" ...
	// Where apps live and where packaged programs unpack to; empty uses the
	// environment and the per-user defaults.
	std::string appsRoot;
	std::string cacheRoot;
};

Role install(const Config &config);
Role role();
// Once per frame. `frameShown`: this frame reached the screen (an app's
// first one tells Home it is ready).
void tick(bool frameShown);
// App: Home asked it to leave (or Home is gone). The shell saves and exits.
bool quitRequested();
// Home: an app is on screen; Home hides and stops drawing until it returns.
bool covered();
// Home: the process host (null otherwise). For tests and diagnostics.
ProcessHost *host();
// Test support: back to an uninstalled state (stops a running app).
void reset();

// The ABI of a process-hosted program: the IPC protocol it speaks. It moves
// only when the line protocol changes incompatibly.
constexpr const char *kProcessAbi = "geaos-process-1";

}  // namespace processPlatform

}  // namespace gea::geaos
