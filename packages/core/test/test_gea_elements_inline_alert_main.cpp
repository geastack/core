#include "native_test_harness.h"

#include "app.h"
#include "ui/document.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 720;
constexpr int kViewportHeight = 1440;
constexpr double kDevicePixelRatio = 2.0;
constexpr const char *kTest = "test_gea_elements_inline_alert_main";
using Pixel = gea::framework::graphics::pixel::native_t;

const char *const kVariants[] = {"info", "success", "warning", "error"};

double g_nowMs = 16;

void advance()
{
	g_nowMs += 16;
	gea::embedded::test::pumpFrame(g_nowMs);
}

bool fail(const char *phase, const std::string &message)
{
	std::fprintf(stderr, "[%s] %s: %s\n", kTest, phase, message.c_str());
	return false;
}

// The alerts are function components, so a re-render replaces their nodes:
// look every node up again for each check instead of holding ids.
int rootOf(const char *id) { return gea::embedded::ui::Document::instance().getElementById(id).id(); }

bool isInside(int node, int ancestor)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int id = node; id >= 0; id = tree.node(id).parent) {
		if (id == ancestor) return true;
	}
	return false;
}

// The visible .partClass nodes inside the alert whose root has this id.
// nodesWithClass skips hidden nodes.
std::vector<int> partsOf(const char *id, const char *partClass)
{
	std::vector<int> out;
	const int root = rootOf(id);
	if (root < 0) return out;
	for (const int node : gea::embedded::test::nodesWithClass(partClass)) {
		if (isInside(node, root)) out.push_back(node);
	}
	return out;
}

int partOf(const char *id, const char *partClass)
{
	const auto parts = partsOf(id, partClass);
	if (parts.size() != 1) {
		std::fprintf(stderr, "[%s] expected one .%s in #%s, got %zu\n", kTest, partClass, id, parts.size());
		return -1;
	}
	return parts[0];
}

std::string partText(const char *id, const char *partClass)
{
	const int node = partOf(id, partClass);
	return node < 0 ? std::string("<missing>") : gea::embedded::test::textContent(node);
}

std::string statusText()
{
	const auto nodes = gea::embedded::test::nodesWithClass("status");
	return nodes.size() == 1 ? gea::embedded::test::textContent(nodes[0]) : std::string("<missing>");
}

bool expectStatus(const char *phase, const char *expected)
{
	const std::string actual = statusText();
	if (actual != expected) return fail(phase, "status \"" + actual + "\", expected \"" + expected + "\"");
	return true;
}

// The root carries the caller's id and class; the box inside it carries the
// alert's own classes, and exactly one variant class.
bool expectVariant(const char *id, const char *callerClass, const char *variant)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = rootOf(id);
	if (root < 0) return fail(id, "no element with this id");
	if (!tree.hasClass(root, callerClass)) return fail(id, "root \"" + tree.className(root) + "\" lacks the caller's class " + callerClass);
	if (tree.hasClass(root, "inline-alert")) return fail(id, "the caller's root also carries .inline-alert");
	const int box = partOf(id, "inline-alert");
	if (box < 0) return false;
	for (const char *other : kVariants) {
		const std::string token = std::string("inline-alert--") + other;
		const bool expected = std::string(other) == variant;
		if (tree.hasClass(box, token) != expected)
			return fail(id, "box \"" + tree.className(box) + "\" " + (expected ? "lacks " : "has ") + token);
	}
	return true;
}

bool expectPartText(const char *id, const char *partClass, const char *expected)
{
	const std::string actual = partText(id, partClass);
	if (actual != expected) return fail(id, std::string(".") + partClass + " is \"" + actual + "\", expected \"" + expected + "\"");
	return true;
}

bool expectNoPart(const char *id, const char *partClass)
{
	const auto parts = partsOf(id, partClass);
	if (!parts.empty()) return fail(id, std::string("has a .") + partClass + " but should not render one");
	return true;
}

// The box's own fill, sampled inside its padding next to the right edge, and
// its left border stripe. Returns false when the alert has no box.
bool boxColors(const char *id, Pixel &fill, Pixel &stripe)
{
	const int box = partOf(id, "inline-alert");
	if (box < 0) return false;
	const auto &layout = gea::embedded::ui::Tree::instance().node(box).layout;
	fill = gea::embedded::test::displayPixelAt(layout.x + layout.width - 6, layout.y + 6);
	stripe = gea::embedded::test::displayPixelAt(layout.x + 3, layout.y + layout.height / 2);
	return true;
}

// Each variant is drawn in its own colors, and its stripe stands out from its
// fill. The severity label carries the meaning; this checks the colors follow
// the variant rather than one shared style.
bool expectVariantColors()
{
	const char *const ids[] = {"alert-info", "alert-success", "alert-warning", "alert-error"};
	std::set<unsigned> fills, stripes;
	bool ok = true;
	for (const char *id : ids) {
		Pixel fill = 0, stripe = 0;
		if (!boxColors(id, fill, stripe)) return false;
		if (fill == stripe) ok = fail(id, "stripe and fill are the same color");
		fills.insert(static_cast<unsigned>(fill));
		stripes.insert(static_cast<unsigned>(stripe));
	}
	if (fills.size() != 4) ok = fail("colors", "the four variants share fill colors (" + std::to_string(fills.size()) + " distinct)");
	if (stripes.size() != 4) ok = fail("colors", "the four variants share stripe colors (" + std::to_string(stripes.size()) + " distinct)");
	Pixel plain = 0, error = 0, unused = 0;
	if (!boxColors("alert-plain", plain, unused) || !boxColors("alert-error", error, unused)) return false;
	if (plain != error) ok = fail("colors", "two error alerts are filled differently");
	return ok;
}

// The root's role, which the browser target reads; the native tree keeps the
// attribute but nothing reads it.
bool expectRole(const char *id, const char *expected)
{
	const int root = rootOf(id);
	if (root < 0) return fail(id, "no element with this id");
	const char *role = gea::embedded::ui::Tree::instance().getAttribute(root, "role");
	if (!role || std::string(role) != expected) return fail(id, std::string("role is \"") + (role ? role : "<none>") + "\", expected \"" + expected + "\"");
	return true;
}

// The parts read top to bottom: severity, title, message, then the buttons.
bool expectPartOrder(const char *id)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const char *const parts[] = {"inline-alert__severity", "inline-alert__title", "inline-alert__message", "inline-alert__actions"};
	int previousBottom = -1;
	for (const char *part : parts) {
		const int node = partOf(id, part);
		if (node < 0) return false;
		const auto &layout = tree.node(node).layout;
		if (layout.y < previousBottom) return fail(id, std::string(".") + part + " is not below the part before it");
		previousBottom = layout.y + layout.height;
	}
	return true;
}

// A long message wraps inside a narrow alert instead of running past it.
bool expectWrapped(const char *id)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = rootOf(id), message = partOf(id, "inline-alert__message");
	if (root < 0 || message < 0) return false;
	const auto &box = tree.node(root).layout;
	const auto &text = tree.node(message).layout;
	const int lineHeight = static_cast<int>(20 * kDevicePixelRatio);
	bool ok = true;
	if (box.width != static_cast<int>(200 * kDevicePixelRatio)) ok = fail(id, "root is " + std::to_string(box.width) + " px wide, expected the caller's 200 px");
	if (text.height < 3 * lineHeight) ok = fail(id, "message is " + std::to_string(text.height) + " px tall, expected at least three wrapped lines");
	if (text.x < box.x || text.x + text.width > box.x + box.width) ok = fail(id, "message runs outside the alert");
	if (box.y + box.height < text.y + text.height) ok = fail(id, "the alert does not grow with the wrapped message");
	return ok;
}

// A long title wraps inside a narrow alert, which grows with it.
bool expectTitleWrapped(const char *id)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = rootOf(id), title = partOf(id, "inline-alert__title"), severity = partOf(id, "inline-alert__severity");
	if (root < 0 || title < 0 || severity < 0) return false;
	const auto &box = tree.node(root).layout;
	const auto &text = tree.node(title).layout;
	const int oneLine = tree.node(severity).layout.height;
	bool ok = true;
	if (box.width != static_cast<int>(200 * kDevicePixelRatio)) ok = fail(id, "root is " + std::to_string(box.width) + " px wide, expected the caller's 200 px");
	if (text.height < 2 * oneLine) ok = fail(id, "title is " + std::to_string(text.height) + " px tall, expected it to wrap");
	if (text.x < box.x || text.x + text.width > box.x + box.width) ok = fail(id, "title runs outside the alert");
	if (box.y + box.height < text.y + text.height) ok = fail(id, "the alert does not grow with the wrapped title");
	return ok;
}

// A button stays inside the actions row, exactly as large as its one-line
// label plus padding and border, and, when endsLine is set, ends its line at
// the row's end.
bool expectButtonFits(const char *id, const char *buttonClass, bool endsLine = true)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int row = partOf(id, "inline-alert__actions"), button = partOf(id, buttonClass);
	if (row < 0 || button < 0) return false;
	const int label = tree.node(button).first_child;
	if (label < 0) return fail(id, std::string(".") + buttonClass + " has no label");
	const auto &r = tree.node(row).layout;
	const auto &b = tree.node(button).layout;
	const auto &t = tree.node(label).layout;
	const std::string name = std::string(".") + buttonClass;
	// 14 px text; a second line would make the label at least twice as tall.
	const int fontPx = static_cast<int>(14 * kDevicePixelRatio);
	// fit-content: 12 px padding and a 1 px border on each side, 6 px
	// padding above and below.
	const int chromeX = static_cast<int>(2 * (12 + 1) * kDevicePixelRatio);
	const int chromeY = static_cast<int>(2 * (6 + 1) * kDevicePixelRatio);
	bool ok = true;
	if (t.height <= 0 || t.height >= 2 * fontPx) ok = fail(id, name + " label is " + std::to_string(t.height) + " px tall; it should fit on one line");
	if (b.width != t.width + chromeX) ok = fail(id, name + " is " + std::to_string(b.width) + " px wide; it should size to its label");
	if (b.height != t.height + chromeY) ok = fail(id, name + " is " + std::to_string(b.height) + " px tall; it should size to its label");
	if (b.x < r.x || b.x + b.width > r.x + r.width) ok = fail(id, name + " runs outside the actions row");
	if (b.y < r.y || b.y + b.height > r.y + r.height) ok = fail(id, name + " runs outside the actions row vertically");
	if (endsLine && b.x + b.width != r.x + r.width) ok = fail(id, name + " does not end at the row's end");
	return ok;
}

// With both buttons, the action comes first. When they fit, they share one
// line and the dismiss button ends it; when they do not, the dismiss button
// wraps below and each ends its own line.
bool expectBothButtons(const char *id, bool oneLine)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int action = partOf(id, "inline-alert__action"), dismiss = partOf(id, "inline-alert__dismiss");
	if (action < 0 || dismiss < 0) return false;
	const auto &a = tree.node(action).layout;
	const auto &d = tree.node(dismiss).layout;
	bool ok = expectButtonFits(id, "inline-alert__dismiss");
	ok = expectButtonFits(id, "inline-alert__action", !oneLine) && ok;
	if (oneLine) {
		if (a.y != d.y) ok = fail(id, "the buttons are not on one line");
		if (a.x + a.width > d.x) ok = fail(id, "the action button does not come before the dismiss button");
	} else {
		if (d.y < a.y + a.height) ok = fail(id, "the dismiss button does not wrap below the action button");
	}
	return ok;
}

// Presses a part of one alert, so two alerts with the same label cannot be
// confused.
bool clickPart(const char *id, const char *partClass)
{
	const int node = partOf(id, partClass);
	if (node < 0 || !gea::embedded::test::dispatchClick(node)) return fail("click", std::string("cannot press .") + partClass + " in #" + id);
	advance();
	return true;
}

bool clickText(const char *label)
{
	if (!gea::embedded::test::clickFirstText(label, true)) return fail("click", std::string("no button labelled ") + label);
	advance();
	return true;
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	gea::framework::app::Application::frame(16);

	bool ok = true;

	// Variants, with the caller's id and class on the root. alert-info passes
	// no variant, so it checks the default.
	ok = expectVariant("alert-info", "alert-info", "info") && ok;
	ok = expectVariant("alert-success", "alert-success", "success") && ok;
	ok = expectVariant("alert-warning", "alert-warning", "warning") && ok;
	ok = expectVariant("alert-error", "alert-error", "error") && ok;
	ok = expectVariant("alert-plain", "alert-plain", "error") && ok;

	// The severity is spelled out, so it does not rely on color. The error
	// alert passes a translated label; the plain one uses the default.
	ok = expectPartText("alert-info", "inline-alert__severity", "Info") && ok;
	ok = expectPartText("alert-success", "inline-alert__severity", "Success") && ok;
	ok = expectPartText("alert-warning", "inline-alert__severity", "Warning") && ok;
	ok = expectPartText("alert-error", "inline-alert__severity", "Hata") && ok;
	ok = expectPartText("alert-plain", "inline-alert__severity", "Error") && ok;
	ok = expectVariantColors() && ok;

	// Errors are announced as alerts on the browser target, the rest as status.
	ok = expectRole("alert-info", "status") && ok;
	ok = expectRole("alert-warning", "status") && ok;
	ok = expectRole("alert-error", "alert") && ok;

	// Title and message are optional and render only when given; alert-success
	// passes title={undefined}.
	ok = expectPartText("alert-info", "inline-alert__title", "Update ready") && ok;
	ok = expectPartText("alert-error", "inline-alert__title", "Yükleme başarısız") && ok;
	ok = expectNoPart("alert-success", "inline-alert__title") && ok;
	ok = expectPartText("alert-error", "inline-alert__message", "Dosya gönderilemedi.") && ok;
	ok = expectPartOrder("alert-error") && ok;
	ok = expectWrapped("alert-warning") && ok;

	// Buttons render only when asked for, with the default or the given label.
	ok = expectPartText("alert-info", "inline-alert__action", "Install") && ok;
	ok = expectNoPart("alert-info", "inline-alert__dismiss") && ok;
	ok = expectNoPart("alert-success", "inline-alert__action") && ok;
	ok = expectPartText("alert-success", "inline-alert__dismiss", "Dismiss") && ok;
	ok = expectPartText("alert-error", "inline-alert__dismiss", "Kapat") && ok;
	ok = expectNoPart("alert-warning", "inline-alert__actions") && ok;
	ok = expectNoPart("alert-plain", "inline-alert__actions") && ok;
	ok = expectButtonFits("alert-info", "inline-alert__action") && ok;
	ok = expectButtonFits("alert-error", "inline-alert__dismiss") && ok;
	ok = expectBothButtons("alert-pair", true) && ok;
	ok = expectBothButtons("alert-both", false) && ok;
	ok = expectTitleWrapped("alert-both") && ok;
	ok = expectPartText("alert-both", "inline-alert__dismiss", "Later") && ok;

	// Empty title, message and action label render nothing; an empty severity
	// or dismiss label falls back to the default instead of going blank.
	ok = expectVariant("alert-empty", "alert-empty", "info") && ok;
	ok = expectPartText("alert-empty", "inline-alert__severity", "Info") && ok;
	ok = expectNoPart("alert-empty", "inline-alert__title") && ok;
	ok = expectNoPart("alert-empty", "inline-alert__message") && ok;
	ok = expectNoPart("alert-empty", "inline-alert__action") && ok;
	ok = expectPartText("alert-empty", "inline-alert__dismiss", "Dismiss") && ok;
	ok = expectStatus("initial", "installs=0 dismissCalls=0") && ok;

	// The action calls onAction once per press and does not dismiss.
	ok = clickPart("alert-info", "inline-alert__action") && ok;
	ok = clickPart("alert-info", "inline-alert__action") && ok;
	ok = expectStatus("after two installs", "installs=2 dismissCalls=0") && ok;
	ok = expectVariant("alert-info", "alert-info", "info") && ok;

	// Dismiss only reports: the app keeps alert-success, so it stays.
	ok = clickPart("alert-success", "inline-alert__dismiss") && ok;
	ok = expectStatus("after dismiss", "installs=2 dismissCalls=1") && ok;
	ok = expectVariant("alert-success", "alert-success", "success") && ok;

	// The error alert's app removes it on onDismiss; the others are untouched.
	ok = clickPart("alert-error", "inline-alert__dismiss") && ok;
	if (rootOf("alert-error") >= 0) ok = fail("after Kapat", "#alert-error is still in the tree");
	if (!nodesWithText("Hata", true).empty()) ok = fail("after Kapat", "the removed alert's label is still shown");
	ok = expectStatus("after Kapat", "installs=2 dismissCalls=1") && ok;
	ok = expectVariant("alert-info", "alert-info", "info") && ok;
	ok = expectVariant("alert-plain", "alert-plain", "error") && ok;

	// A variant that the app changes re-renders with the new class, label and
	// role.
	ok = clickText("Downgrade") && ok;
	ok = expectVariant("alert-plain", "alert-plain", "warning") && ok;
	ok = expectPartText("alert-plain", "inline-alert__severity", "Warning") && ok;
	ok = expectRole("alert-plain", "status") && ok;

	// Each action reaches only its own alert's callback.
	ok = clickPart("alert-both", "inline-alert__action") && ok;
	ok = clickPart("alert-pair", "inline-alert__action") && ok;
	ok = expectStatus("after two other actions", "installs=112 dismissCalls=1") && ok;

	// A dismiss button without onDismiss does nothing when pressed.
	ok = clickPart("alert-both", "inline-alert__dismiss") && ok;
	ok = clickPart("alert-empty", "inline-alert__dismiss") && ok;
	ok = expectStatus("after Later", "installs=112 dismissCalls=1") && ok;
	ok = expectVariant("alert-both", "alert-both", "warning") && ok;

	if (!ok) {
		dumpTree(kTest);
		return 1;
	}
	return 0;
}
