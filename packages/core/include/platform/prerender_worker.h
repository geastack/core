// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace gea::platform::prerender {

struct Request {
	std::string input;
	std::string out;
	int width = 1920;
	int height = 1080;
	double dpr = 1;
	bool reusePage = false;
};

using Capture = void (*)(const Request &request, bool reuse);

bool validViewport(double width, double height, double dpr);
int runWorker(Capture capture);

} // namespace gea::platform::prerender
