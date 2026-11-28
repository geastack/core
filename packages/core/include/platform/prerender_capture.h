// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ui/document.h"
#include "ui/tree_internal.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace gea::platform::prerender {

struct CaptureRegion {
	int x = 0;
	int y = 0;
	int width;
	int height;
};

// Preserve document layout, then paint only the selected subtree into the viewport.
inline CaptureRegion prepareCaptureRegion(int width, int height, const std::string &selector,
                                         bool transparent)
{
	if (selector.empty() && !transparent) return {0, 0, width, height};

	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = selector.empty() ? tree.mountedRoot() :
	    gea::embedded::ui::Document::instance().querySelector(selector).id();
	if (root < 0) throw std::invalid_argument("Capture selector did not match an element");

	CaptureRegion region{0, 0, width, height};
	if (!selector.empty()) {
		const auto &box = tree.node(root).layout;
		const int x = std::clamp<int>(box.x, 0, width);
		const int y = std::clamp<int>(box.y, 0, height);
		const int right = std::clamp<int>(box.x + box.width, 0, width);
		const int bottom = std::clamp<int>(box.y + box.height, 0, height);
		if (right <= x || bottom <= y) {
			throw std::invalid_argument("Capture element has no area inside the viewport");
		}
		region = {x, y, right - x, bottom - y};
	}
	tree.paintCapture(root, transparent);
	return region;
}

} // namespace gea::platform::prerender
