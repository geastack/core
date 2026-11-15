// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

class LayoutSnapshot {
public:
	static void capture();
	static void captureNodes(const int *ids, int count, int subtreeRoot);
	static void markChangesDirty();
};

}  // namespace gea::embedded::ui
