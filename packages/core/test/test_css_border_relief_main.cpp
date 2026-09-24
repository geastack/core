// SPDX-License-Identifier: Apache-2.0
#include "native_test_harness.h"
#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <cstdio>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
namespace pixel = gea::framework::graphics::pixel;

bool expectPixel(int x, int y, uint16_t expected, const char *label)
{
	const auto actual = displayPixelAt(x, y);
	if (actual == expected) return true;
	std::fprintf(stderr, "[border-relief] %s (%d,%d): expected %04x, got %04x\n", label, x, y, expected, actual);
	return false;
}

int main()
{
	resetNativeHost(); setNativeDisplaySize(400, 180); setViewportMetrics(400, 180, 1);
	auto &sheet = StyleSheet::instance();
	sheet.clear();
	sheet.registerSelectorRule(".groove", "border", "10px groove");
	sheet.registerSelectorRule(".ridge", "border", "10px ridge");
	sheet.registerSelectorRule(".inset", "border", "10px inset");
	sheet.registerSelectorRule(".outset", "border", "10px outset");
	auto root = Document::instance().createView(); root.setTagName("div");
	root.style().width(400); root.style().height(180); root.style().backgroundColor(0xffff);
	root.style().setProperty("color", "#000000");
	const char *classes[] = {"groove", "groove", "ridge", "inset", "outset"};
	int ids[5];
	for (int i = 0; i < 5; ++i) {
		auto box = Document::instance().createView(); box.setTagName("div"); ids[i] = box.id();
		box.classList().set(classes[i]); box.style().setProperty("position", "absolute");
		box.style().left(10 + 75 * i); box.style().top(10); box.style().width(40); box.style().height(40);
		root.appendChild(box);
	}
	Document::instance().mount(root, 400, 180);
	bool ok = true;
	const auto light = pixel::nativeColor(102, 102, 102), dark = pixel::nativeColor(0, 0, 0);
	for (int i = 0; i < 5; ++i) {
		const int x = 10 + i * 75;
		const bool outerInset = i < 2 || i == 3;
		const bool innerInset = i == 2 || i == 3;
		ok &= expectPixel(x + 30, 12, outerInset ? dark : light, "outer top");
		ok &= expectPixel(x + 30, 17, innerInset ? dark : light, "inner top");
		ok &= expectPixel(x + 30, 67, outerInset ? light : dark, "outer bottom");
		ok &= expectPixel(x + 30, 62, innerInset ? light : dark, "inner bottom");
		ok &= expectPixel(x + 2, 40, outerInset ? dark : light, "outer left");
		ok &= expectPixel(x + 57, 40, outerInset ? light : dark, "outer right");
		ok &= expectPixel(x + 30, 40, 0xffff, "interior stays transparent");
		ok &= expectPixel(x - 1, 40, 0xffff, "outside stays untouched");
		ok &= expectPixel(x + 60, 40, 0xffff, "outside right stays untouched");
		ok &= expectPixel(x + 30, 70, 0xffff, "outside bottom stays untouched");
	}
	// Inline declarations and class plans share the same shaded border state.
	NodeHandle(ids[0]).style().setProperty("border", "10px ridge");
	Document::instance().refresh(root, 400, 180);
	ok &= expectPixel(40, 12, light, "inline style overrides class");
	NodeHandle(ids[0]).style().removeProperty("border");
	Document::instance().refresh(root, 400, 180);
	ok &= expectPixel(40, 12, dark, "removing inline style restores class");
	NodeHandle(ids[0]).style().setProperty("border-top", "10px solid #000000");
	Document::instance().refresh(root, 400, 180);
	ok &= expectPixel(40, 17, dark, "side shorthand resets relief");
	ok &= expectPixel(67, 40, light, "side reset leaves other edges shaded");
	NodeHandle(ids[1]).style().setProperty("transform", "translate(0px,80px)");
	Document::instance().refresh(root, 400, 180);
	ok &= expectPixel(115, 97, light, "retained transform carries inner groove");
	ok &= expectPixel(115, 17, 0xffff, "retained transform clears old edge");
	if (ok) std::puts("[border-relief] ALL PASS");
	return ok ? 0 : 1;
}
