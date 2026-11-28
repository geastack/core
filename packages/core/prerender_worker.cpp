// SPDX-License-Identifier: Apache-2.0
#include "platform/prerender_worker.h"

#include "gea_runtime.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gea::platform::prerender {

bool validViewport(double width, double height, double dpr)
{
	return width >= 1 && width <= 8192 && std::trunc(width) == width &&
	       height >= 1 && height <= 8192 && std::trunc(height) == height &&
	       std::isfinite(dpr) && dpr >= 0.1 && dpr <= 8;
}

static Request parseRequest(const std::string &line)
{
	gea::json::Reader reader(line);
	if (!reader.enterObject()) throw std::invalid_argument("Expected a capture request object");

	Request request;
	double width = request.width;
	double height = request.height;
	std::string key;
	do {
		reader.nextKey(key);
		if (key == "input") request.input = reader.readStringField();
		else if (key == "out") request.out = reader.readStringField();
		else if (key == "width") width = reader.readNumberField();
		else if (key == "height") height = reader.readNumberField();
		else if (key == "dpr") request.dpr = reader.readNumberField();
		else if (key == "reusePage") request.reusePage = reader.readBoolField();
		else if (key == "selector") request.selector = reader.readStringField();
		else if (key == "transparent") request.transparent = reader.readBoolField();
		else reader.skipValue();
	} while (reader.consumeIf(','));
	if (!reader.consumeIf('}')) throw std::invalid_argument("Expected end of capture request");
	reader.finish();

	if (request.input.empty() || request.out.empty() || request.input.find('\0') != std::string::npos ||
	    request.out.find('\0') != std::string::npos || !validViewport(width, height, request.dpr)) {
		throw std::invalid_argument("Invalid capture paths or viewport");
	}
	request.width = static_cast<int>(width);
	request.height = static_cast<int>(height);
	return request;
}

namespace detail {

inline FILE *openProtocolOutput()
{
	std::fflush(stdout);
#ifdef _WIN32
	const int fd = _dup(_fileno(stdout));
	if (fd < 0) return nullptr;
	FILE *stream = _fdopen(fd, "w");
	if (!stream) { _close(fd); return nullptr; }
	if (_dup2(_fileno(stderr), _fileno(stdout)) < 0) { std::fclose(stream); return nullptr; }
#else
	const int fd = dup(fileno(stdout));
	if (fd < 0) return nullptr;
	FILE *stream = fdopen(fd, "w");
	if (!stream) { close(fd); return nullptr; }
	if (dup2(fileno(stderr), fileno(stdout)) < 0) { std::fclose(stream); return nullptr; }
#endif
	return stream;
}

inline std::string failure(const std::string &message)
{
	std::string reply = "{\"ok\":false,\"error\":";
	gea_json_write(reply, message);
	reply += '}';
	return reply;
}

} // namespace detail

int runWorker(Capture capture)
{
	FILE *protocol = detail::openProtocolOutput();
	if (!protocol) return 1;

	std::optional<Request> mounted;
	std::string line;
	while (std::getline(std::cin, line)) {
		std::string reply;
		try {
			auto request = parseRequest(line);
			const bool reuse = request.reusePage && mounted && request.selector.empty() && !request.transparent &&
			                   mounted->selector.empty() && !mounted->transparent && request.input == mounted->input &&
			                   request.width == mounted->width && request.height == mounted->height &&
			                   request.dpr == mounted->dpr;
			const auto started = std::chrono::steady_clock::now();
			capture(request, reuse);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			mounted = std::move(request);
			reply = "{\"ok\":true,\"ms\":" + std::to_string(ms) + ",\"reusedPage\":" + (reuse ? "true}" : "false}");
		} catch (const std::exception &error) {
			mounted.reset();
			reply = detail::failure(error.what());
		} catch (...) {
			mounted.reset();
			reply = detail::failure("Capture failed");
		}
		if (std::fprintf(protocol, "%s\n", reply.c_str()) < 0 || std::fflush(protocol) != 0) {
			std::fclose(protocol);
			return 1;
		}
	}
	return std::fclose(protocol) != 0;
}

} // namespace gea::platform::prerender
