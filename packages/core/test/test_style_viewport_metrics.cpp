#include "native_test_harness.h"

#include "display.h"
#include "host/display.h"
#include "image.h"
#include "pixel.h"
#include "css/engine.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[test_style_viewport_metrics] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

bool expectClose(double actual, double expected, const char *label)
{
	if (std::fabs(actual - expected) < 0.0001) return true;
	std::fprintf(stderr, "[test_style_viewport_metrics] %s expected %.4f, got %.4f\n", label, expected, actual);
	return false;
}

int pixelLuma(std::uint16_t pixel)
{
	int r = 0, g = 0, b = 0;
	gea::framework::graphics::pixel::unpackRgb565(pixel, &r, &g, &b);
	r = (r * 255 + 15) / 31;
	g = (g * 255 + 31) / 63;
	b = (b * 255 + 15) / 31;
	return (r * 77 + g * 150 + b * 29 + 128) / 256;
}

int findDirectChildByTag(int parent, const char *tag)
{
	gea::embedded::ui::Tree &tree = gea::embedded::ui::Tree::instance();
	if (parent < 0 || parent >= tree.nodeCount()) return -1;
	for (int child = tree.node(parent).first_child; child >= 0; child = tree.node(child).next_sibling) {
		if (std::strcmp(tree.tagName(child), tag) == 0) return child;
	}
	return -1;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(410, 502, 1.5);

	const int nodeId = Tree::instance().createView();
	const int childId = Tree::instance().createView();
	NodeHandle node(nodeId);
	node.appendChild(NodeHandle(childId));
	StyleSheet::instance().registerRule("probe", "width", "100vw");
	StyleSheet::instance().registerRule("probe", "height", "100vh");
	StyleSheet::instance().registerRule("probe", "padding", "10px");
	StyleSheet::instance().registerRule("probe", "margin", "1px 2px 3px 4px");
	StyleSheet::instance().registerRule("probe", "border-width", "0.5px");
	StyleSheet::instance().registerRule("probe", "font-size", "24px");
	StyleSheet::instance().registerRule("probe", "line-height", "1.25");
	StyleSheet::instance().registerRule("probe", "gap", "50vw");
	StyleSheet::instance().registerRule("probe", "min-width", "33");
	StyleSheet::instance().registerRule("probe", "flex", "1");
	StyleSheet::instance().registerRule("probe", "z-index", "7");
	node.classList().add("probe");

	const auto &style = Tree::instance().node(nodeId).style;
	if (!expectEqual(style.width, 410, "100vw width")) return 1;
	if (!expectEqual(style.height, 502, "100vh height")) return 1;
	if (!expectEqual(style.padding[0], 15, "10px padding-top at 1.5x")) return 1;
	if (!expectEqual(style.margin[0], 2, "1px margin-top")) return 1;
	if (!expectEqual(style.margin[1], 3, "2px margin-right at 1.5x")) return 1;
	if (!expectEqual(style.margin[2], 5, "3px margin-bottom at 1.5x")) return 1;
	if (!expectEqual(style.margin[3], 6, "4px margin-left at 1.5x")) return 1;
	if (!expectEqual(style.border_width, 1, "0.5px border-width")) return 1;
	if (!expectEqual(style.font_size, 36, "24px font-size at 1.5x")) return 1;
	if (!expectEqual(style.line_height, 45, "1.25 line-height at 1.5x")) return 1;
	if (!expectEqual(Tree::instance().node(childId).style.line_height, 45, "inherited line-height")) return 1;
	if (!expectEqual(style.gap, 205, "50vw gap")) return 1;
	if (!expectEqual(style.min_width, 33, "legacy unitless min-width")) return 1;
	if (!expectEqual(style.flex, 1, "flex")) return 1;
	if (!expectEqual(style.z_index, 7, "z-index")) return 1;

	gea::host::Display.setDevicePixelRatio(2);
	if (!expectClose(gea::host::Display.getDevicePixelRatio(), 2.0, "Display device pixel ratio")) return 1;

	const auto &rescaledStyle = Tree::instance().node(nodeId).style;
	if (!expectEqual(rescaledStyle.padding[0], 20, "Display-rescaled padding-top")) return 1;
	if (!expectEqual(rescaledStyle.font_size, 48, "Display-rescaled font-size")) return 1;

	gea::host::Display.setSupportedOrientations(std::vector<std::string>{"portrait", "landscape"});
	gea::host::Display.setOrientation("landscape-primary");
	if (!expectEqual(static_cast<int>(gea::host::Display.width()), gea::platform::display::kHeight, "landscape Display.width")) return 1;
	if (!expectEqual(static_cast<int>(gea::host::Display.height()), gea::platform::display::kWidth, "landscape Display.height")) return 1;
	if (!expectEqual(Tree::instance().node(nodeId).style.width, gea::platform::display::kHeight, "landscape 100vw width")) return 1;
	if (!expectEqual(Tree::instance().node(nodeId).style.height, gea::platform::display::kWidth, "landscape 100vh height")) return 1;
	gea::host::Display.setAutoRotate(true);
	gea::framework::display::DisplayBackend::updateAutoRotation(-9.8, 0.0, 0.0);
	if (!expectEqual(static_cast<int>(gea::host::Display.width()), gea::platform::display::kHeight, "auto-rotated landscape width")) return 1;
	gea::framework::display::DisplayBackend::updateAutoRotation(0.0, 9.8, 0.0);
	if (!expectEqual(static_cast<int>(gea::host::Display.width()), gea::platform::display::kWidth, "auto-rotated portrait width")) return 1;
	gea::host::Display.setAutoRotate(false);
	gea::host::Display.setOrientation("portrait-primary");
	if (!expectEqual(rescaledStyle.line_height, 60, "Display-rescaled line-height")) return 1;

	StyleSheet::instance().applyNumberProperty(node, "width", 42);
	StyleSheet::instance().applyNumberProperty(node, "font-size", 12);
	StyleSheet::instance().applyNumberProperty(node, "line-height", 1.5);
	StyleSheet::instance().applyNumberProperty(node, "flex", 3);
	StyleSheet::instance().applyNumberProperty(node, "z-index", 11);
	StyleSheet::instance().applyProperty(node,
	                                     "mask-image",
	                                     "linear-gradient(to right, #000 0, #000 calc(100% - 14.667px), transparent 100%)");

	const auto &inlineStyle = Tree::instance().node(nodeId).style;
	if (!expectEqual(inlineStyle.width, 42, "legacy numeric width")) return 1;
	if (!expectEqual(inlineStyle.font_size, 12, "legacy numeric font-size")) return 1;
	if (!expectEqual(inlineStyle.line_height, 18, "legacy numeric line-height")) return 1;
	if (!expectEqual(inlineStyle.flex, 3, "numeric flex")) return 1;
	if (!expectEqual(inlineStyle.z_index, 11, "numeric z-index")) return 1;
	if (!expectEqual(inlineStyle.mask_right_fade_width, 29, "right-edge mask fade width")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(410, 502, 1.5);

	const int wrapId = Tree::instance().createView();
	const int cubeId = Tree::instance().createView();
	const int faceId = Tree::instance().createView();
	NodeHandle wrap(wrapId);
	NodeHandle cube(cubeId);
	NodeHandle face(faceId);
	wrap.appendChild(cube);
	cube.appendChild(face);

	StyleSheet::instance().registerRule("cube-wrap", "--cube-size", "clamp(154px, 34vw, 260px)");
	StyleSheet::instance().registerRule("cube", "width", "var(--cube-size)");
	StyleSheet::instance().registerRule("cube", "height", "var(--cube-size)");
	StyleSheet::instance().registerRule("cube-face", "inset", "0");
	StyleSheet::instance().registerRule("cube-face", "border", "1px solid rgba(255, 246, 232, 0.42)");
	StyleSheet::instance().registerRule("cube-face", "background", "linear-gradient(145deg, rgba(255, 241, 118, 0.92), rgba(255, 128, 97, 0.9))");
	StyleSheet::instance().registerRule("cube-face--right", "transform", "rotateY(90deg) translateZ(calc(var(--cube-size) / 2))");
	wrap.classList().add("cube-wrap");
	cube.classList().add("cube");
	face.classList().add("cube-face");
	face.classList().add("cube-face--right");

	const auto &cubeStyle = Tree::instance().node(cubeId).style;
	const auto &faceStyle = Tree::instance().node(faceId).style;
	if (!expectEqual(cubeStyle.width, 231, "var/clamp cube width")) return 1;
	if (!expectEqual(cubeStyle.height, 231, "var/clamp cube height")) return 1;
	if (!expectEqual(faceStyle.pos_offsets[0], 0, "inset top")) return 1;
	if (!expectEqual(faceStyle.pos_offsets[1], 0, "inset right")) return 1;
	if (!expectEqual(faceStyle.pos_offsets[2], 0, "inset bottom")) return 1;
	if (!expectEqual(faceStyle.pos_offsets[3], 0, "inset left")) return 1;
	// CSS Values 4 snaps the 1.5 device-pixel stroke down to one whole pixel.
	if (!expectEqual(faceStyle.border_width, 1, "border shorthand width at DPR 1.5")) return 1;
	if (!expectEqual(static_cast<int>(faceStyle.border_alpha), 107, "border shorthand alpha")) return 1;
	if (!expectEqual(faceStyle.has_bg, 1, "gradient background fallback")) return 1;
	if (!expectEqual(rstyle(faceStyle).transform_rotate_y, 900, "rotateY transform")) return 1;
	if (!expectEqual(rstyle(faceStyle).transform_translate_z, 116, "translateZ calc(var / 2)")) return 1;

	StyleSheet::instance().registerKeyframeRule("cube-spin", 0, "transform", "rotateX(-18deg) rotateY(24deg) rotateZ(0deg)");
	StyleSheet::instance().registerKeyframeRule("cube-spin", 1000, "transform", "rotateX(342deg) rotateY(384deg) rotateZ(0deg)");
	StyleSheet::instance().registerRule("cube", "animation", "cube-spin 10s cubic-bezier(0.62, 0, 0.28, 1) infinite");
	gea::css::AnimationEngine::instance().clear();
	StyleSheet::instance().startCssAnimations(0);
	gea::css::AnimationEngine::instance().tick(0);

	const auto &animatedCubeStyle = Tree::instance().node(cubeId).style;
	if (!expectEqual(static_cast<int>(gea::css::AnimationEngine::instance().count()), 3, "cube-spin animated properties")) return 1;
	if (!expectEqual(rstyle(animatedCubeStyle).transform_rotate_x, -180, "cube-spin rotateX initial")) return 1;
	if (!expectEqual(rstyle(animatedCubeStyle).transform_rotate_y, 240, "cube-spin rotateY initial")) return 1;
	if (!expectEqual(rstyle(animatedCubeStyle).transform_rotate, 0, "cube-spin rotateZ initial")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(24, 112);
	gea::embedded::ui::setViewportMetrics(24, 112, 1.0);

	const int gradientProbeId = Tree::instance().createView();
	NodeHandle gradientProbe(gradientProbeId);
	StyleSheet::instance().registerRule("late-stop", "background", "linear-gradient(to bottom, #000000 0%, #808080 50%, #ffffff 150%)");
	StyleSheet::instance().registerRule("late-stop", "width", "20");
	StyleSheet::instance().registerRule("late-stop", "height", "100");
	gradientProbe.classList().add("late-stop");
	auto &gradientProbeNode = Tree::instance().node(gradientProbeId);
	gradientProbeNode.layout.x = 2;
	gradientProbeNode.layout.y = 2;
	gradientProbeNode.layout.width = 20;
	gradientProbeNode.layout.height = 100;
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	ViewRenderer::recordBox(gradientProbeNode);
	DisplayList::instance().replay();
	const int lateStopLuma = pixelLuma(displayPixelAt(12, 100));
	if (lateStopLuma > 210) {
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected >100%% gradient stop to remain below white at the bottom edge, got luma=%d\n",
		             lateStopLuma);
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	const int staticBgId = Tree::instance().createView();
	StyleSheet::instance().registerStaticBackgroundRule(
	    StaticStyleSelectorKind::Class,
	    "static-bg",
	    {1800, {10, 20, 30, 220}, {40, 50, 60, 180}, {70, 80, 90, 160}, 420, 1200, true},
	    {900, {1, 2, 3, 64}, {4, 5, 6, 96}, {7, 8, 9, 128}, 500, 1000, true},
	    true,
	    {true, {StaticStyleLengthUnit::Px, 1}, {255, 241, 118, 41}},
	    {true, {StaticStyleLengthUnit::Px, 2}, {255, 241, 118, 33}});
	StyleSheet::instance().registerStaticBackgroundSizeRule(
	    StaticStyleSelectorKind::Class,
	    "static-bg",
	    {StaticStyleLengthUnit::Px, 38},
	    {StaticStyleLengthUnit::Px, 39});
	NodeHandle(staticBgId).classList().set("static-bg");
	const auto &staticBgStyle = Tree::instance().node(staticBgId).style;
	const auto &staticBgRare = rstyle(staticBgStyle);
	if (!expectEqual(staticBgStyle.has_bg, 1, "static background has_bg")) return 1;
	if (!expectEqual(staticBgStyle.bg_fill, 1, "static background fill")) return 1;
	if (!expectEqual(static_cast<int>(staticBgStyle.bg_alpha), 0, "static image shorthand has transparent base")) return 1;
	if (!expectEqual(static_cast<int>(staticBgRare.bg_gradient_from_color),
	                 static_cast<int>(gea::framework::graphics::pixel::nativeColor(10, 20, 30)),
	                 "static background first image stop")) return 1;
	if (!expectEqual(staticBgRare.bg_gradient_has_mid, 1, "static background mid stop")) return 1;
	if (!expectEqual(staticBgRare.bg_gradient_mid_stop, 420, "static background mid stop value")) return 1;
	if (!expectEqual(staticBgRare.bg_gradient_to_stop, 1200, "static background late stop value")) return 1;
	if (!expectEqual(staticBgRare.bg_overlay_gradient, 1, "static background overlay")) return 1;
	if (!expectEqual(staticBgRare.bg_grid_axes, 3, "static background grid axes")) return 1;
	if (!expectEqual(staticBgRare.bg_grid_line_x, 1, "static background grid line x")) return 1;
	if (!expectEqual(staticBgRare.bg_grid_line_y, 2, "static background grid line y")) return 1;
	if (!expectEqual(staticBgRare.bg_grid_step_x, 38, "static background-size x")) return 1;
	if (!expectEqual(staticBgRare.bg_grid_step_y, 39, "static background-size y")) return 1;
	const int firstRareStyle = staticBgStyle.rare_style;
	int maxRareStyle = firstRareStyle;
	for (int i = 0; i < 8; ++i) {
		StyleSheet::instance().recomputeSubtree(staticBgId);
		const int rareStyle = Tree::instance().node(staticBgId).style.rare_style;
		if (rareStyle < 0) {
			std::fprintf(stderr, "[test_style_viewport_metrics] static background recompute lost RareStyle handle\n");
			return 1;
		}
		if (rareStyle > maxRareStyle) maxRareStyle = rareStyle;
	}
	if (maxRareStyle > firstRareStyle + 1) {
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] static background recompute leaked RareStyle handles, first=%d max=%d\n",
		             firstRareStyle,
		             maxRareStyle);
		return 1;
	}

	const int runtimeLengthId = Tree::instance().createView();
	StyleSheet::instance().registerStaticLengthRule(
	    StaticStyleSelectorKind::Class,
	    "runtime-length",
	    StaticStyleLengthProperty::Width,
	    StaticStyleLengthUnit::Vw,
	    50);
	StyleSheet::instance().registerStaticLengthRule(
	    StaticStyleSelectorKind::Class,
	    "runtime-length",
	    StaticStyleLengthProperty::Height,
	    StaticStyleLengthUnit::Vh,
	    25);
	StyleSheet::instance().registerStaticLengthRule(
	    StaticStyleSelectorKind::Class,
	    "runtime-length",
	    StaticStyleLengthProperty::Perspective,
	    StaticStyleLengthUnit::Vh,
	    50);
	NodeHandle(runtimeLengthId).classList().set("runtime-length");
	const auto &runtimeLengthStyle = Tree::instance().node(runtimeLengthId).style;
	if (!expectEqual(runtimeLengthStyle.width, 12, "runtime cached vw width")) return 1;
	if (!expectEqual(runtimeLengthStyle.height, 28, "runtime cached vh height")) return 1;
	if (!expectEqual(rstyle(runtimeLengthStyle).perspective, 56, "runtime cached vh perspective")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	StyleSheet::instance().registerStaticSelectorRule(".combo.extra", "width", "42px");
	const int comboOnlyId = Tree::instance().createView();
	const int comboExtraId = Tree::instance().createView();
	NodeHandle(comboOnlyId).classList().set("combo");
	NodeHandle(comboExtraId).classList().set("combo extra");
	if (!expectEqual(Tree::instance().node(comboOnlyId).style.width, kUnset, "compound selector missing class")) return 1;
	if (!expectEqual(Tree::instance().node(comboExtraId).style.width, 42, "compound selector extra class")) return 1;

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	Node glow{};
	glow.type = NodeType::View;
	glow.layout.x = 2;
	glow.layout.y = 2;
	glow.layout.width = 20;
	glow.layout.height = 100;
	glow.style.has_bg = 1;
	glow.style.bg_fill = 1;
	glow.style.bg_alpha = 255;
	rstyleMut(glow.style).bg_gradient_from_color = 0;
	rstyleMut(glow.style).bg_gradient_to_color = 0;
	rstyleMut(glow.style).bg_gradient_from_alpha = 255;
	rstyleMut(glow.style).bg_gradient_to_alpha = 255;
	rstyleMut(glow.style).bg_gradient_angle = 1800;
	rstyleMut(glow.style).bg_gradient_to_stop = 1000;
	rstyleMut(glow.style).bg_radial_gradient = 1;
	rstyleMut(glow.style).bg_radial_gradient_from_color = gea::framework::graphics::pixel::rgb565FromRgb888(255, 200, 100);
	rstyleMut(glow.style).bg_radial_gradient_to_color = 0;
	rstyleMut(glow.style).bg_radial_gradient_from_alpha = 128;
	rstyleMut(glow.style).bg_radial_gradient_to_alpha = 0;
	rstyleMut(glow.style).bg_radial_gradient_stop = 1000;
	rstyleMut(glow.style).bg_radial_gradient_cx = 500;
	rstyleMut(glow.style).bg_radial_gradient_cy = 0;
	rstyleMut(glow.style).bg_radial_gradient_rx = 1000;
	rstyleMut(glow.style).bg_radial_gradient_ry = 1000;
	ViewRenderer::recordBox(glow);
	DisplayList::instance().replay();
	const int radialTransparentLuma = pixelLuma(displayPixelAt(12, 52));
	if (radialTransparentLuma < 32) {
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected radial fade to transparent to preserve the source hue while alpha fades, got luma=%d\n",
		             radialTransparentLuma);
		return 1;
	}

	gea::platform::display::Display::clearNoFlush();
	const std::uint16_t scalePixels[4] = {
	    gea::framework::graphics::pixel::rgb565FromRgb888(255, 0, 0),
	    gea::framework::graphics::pixel::rgb565FromRgb888(0, 0, 255),
	    gea::framework::graphics::pixel::rgb565FromRgb888(0, 255, 0),
	    gea::framework::graphics::pixel::rgb565FromRgb888(255, 255, 255),
	};
	const std::uint8_t scaleAlpha[4] = {255, 255, 255, 255};
	gea::platform::display::Display::blitImageScaled(scalePixels, scaleAlpha, 2, 2, 2, 2, 4, 4);
	const std::uint16_t scaledSample = displayPixelAt(3, 3);
	if (scaledSample == scalePixels[0] || scaledSample == scalePixels[1] ||
	    scaledSample == scalePixels[2] || scaledSample == scalePixels[3]) {
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected scaled images to be filtered between source pixels, got exact source color=0x%04x\n",
		             scaledSample);
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 120, 1.0);

	const int panelId = Tree::instance().createView();
	const int firstId = Tree::instance().createView();
	const int secondId = Tree::instance().createView();
	const int thirdId = Tree::instance().createView();
	const int fourthId = Tree::instance().createView();
	NodeHandle panel(panelId);
	panel.appendChild(NodeHandle(firstId));
	panel.appendChild(NodeHandle(secondId));
	panel.appendChild(NodeHandle(thirdId));
	panel.appendChild(NodeHandle(fourthId));
	Tree::instance().mount(panelId, 300, 120);

	StyleSheet::instance().registerSelectorRule(":root", "--ink", "#000000");
	StyleSheet::instance().registerSelectorRule(".panel.cloud", "--panel-radius", "12");
	StyleSheet::instance().registerSelectorRule(".panel.cloud", "border-radius", "var(--panel-radius)");
	StyleSheet::instance().registerSelectorRule(".panel.cloud", "color", "var(--ink)");
	StyleSheet::instance().registerSelectorRule(".panel .value", "opacity", "72%");
	StyleSheet::instance().registerSelectorRule(".panel > .value:first-child", "z-index", "9");
	StyleSheet::instance().registerSelectorRule(".panel::before", "content", "\"\"");
	StyleSheet::instance().registerSelectorRule(".panel::before", "position", "absolute");
	StyleSheet::instance().registerSelectorRule(".panel::before", "left", "0");
	StyleSheet::instance().registerSelectorRule(".panel::before", "top", "0");
	StyleSheet::instance().registerSelectorRule(".panel::before", "width", "24");
	StyleSheet::instance().registerSelectorRule(".panel::before", "height", "8");
	StyleSheet::instance().registerSelectorRule(".panel::before", "background", "var(--ink)");
	StyleSheet::instance().registerSelectorRule(".panel", "display", "grid");
	StyleSheet::instance().registerSelectorRule(".panel", "width", "300");
	StyleSheet::instance().registerSelectorRule(".panel", "height", "120");
	StyleSheet::instance().registerSelectorRule(".panel", "grid-template-columns", "repeat(3, minmax(0, 1fr))");
	StyleSheet::instance().registerSelectorRule(".panel", "grid-template-rows", "40 1fr");
	StyleSheet::instance().registerSelectorRule(".panel", "gap", "10");

	panel.classList().set("panel cloud");
	NodeHandle(firstId).classList().add("value");
	NodeHandle(secondId).classList().add("value");
	NodeHandle(thirdId).classList().add("value");
	NodeHandle(fourthId).classList().add("value");
	Tree::instance().computeLayout(panelId, 300, 120);

	const auto &panelStyle = Tree::instance().node(panelId).style;
	if (!expectEqual(panelStyle.text_color, 0, ":root var color")) return 1;
	if (!expectEqual(panelStyle.border_radius[0], 12, "compound selector var border radius")) return 1;
	if (!expectEqual(Tree::instance().node(secondId).style.opacity, 72, "descendant selector opacity")) return 1;
	if (!expectEqual(Tree::instance().node(firstId).style.z_index, 9, "child + first-child selector")) return 1;
	if (!expectEqual(panelStyle.display, 2, "grid display value")) return 1;

	const int beforeId = findDirectChildByTag(panelId, "::before");
	if (!expectEqual(beforeId >= 0 ? 1 : 0, 1, "materialized ::before node")) return 1;
	const auto &beforeStyle = Tree::instance().node(beforeId).style;
	if (!expectEqual(beforeStyle.width, 24, "::before width")) return 1;
	if (!expectEqual(beforeStyle.height, 8, "::before height")) return 1;
	if (!expectEqual(beforeStyle.has_bg, 1, "::before var background")) return 1;

	const auto &firstLayout = Tree::instance().node(firstId).layout;
	const auto &secondLayout = Tree::instance().node(secondId).layout;
	const auto &fourthLayout = Tree::instance().node(fourthId).layout;
	if (!expectEqual(firstLayout.x, 0, "grid first child x")) return 1;
	if (!expectEqual(firstLayout.y, 0, "grid first child y")) return 1;
	if (!expectEqual(firstLayout.width, 93, "grid first child width")) return 1;
	if (!expectEqual(firstLayout.height, 40, "grid first child height")) return 1;
	if (!expectEqual(secondLayout.x, 103, "grid second child x")) return 1;
	if (!expectEqual(fourthLayout.y, 50, "grid fourth child row y")) return 1;
	if (!expectEqual(fourthLayout.height, 70, "grid fourth child fr row height")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(120, 80, 1.0);

	const int activeChipId = Tree::instance().createView();
	NodeHandle activeChip(activeChipId);
	Tree::instance().mount(activeChipId, 120, 80);
	StyleSheet::instance().registerRule("city-chip", "width", "60");
	StyleSheet::instance().registerRule("city-chip", "height", "20");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "content", "\"\"");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "position", "absolute");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "left", "0");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "right", "0");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "bottom", "0");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "height", "2");
	StyleSheet::instance().registerSelectorRule(".city-chip.is-active::after", "background", "#ffffff");
	activeChip.classList().set("city-chip is-active");
	if (!expectEqual(findDirectChildByTag(activeChipId, "::after") >= 0 ? 1 : 0, 1, "active chip ::after materialized")) return 1;
	activeChip.classList().set("city-chip");
	if (!expectEqual(findDirectChildByTag(activeChipId, "::after") >= 0 ? 1 : 0, 0, "inactive chip ::after removed")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 180, 1.0);

	const int flexId = Tree::instance().createView();
	const int flexFirstId = Tree::instance().createView();
	const int flexSecondId = Tree::instance().createView();
	NodeHandle flex(flexId);
	NodeHandle(flexFirstId).style().width(50);
	NodeHandle(flexFirstId).style().height(20);
	NodeHandle(flexSecondId).style().width(50);
	NodeHandle(flexSecondId).style().height(20);
	flex.appendChild(NodeHandle(flexFirstId));
	flex.appendChild(NodeHandle(flexSecondId));
	Tree::instance().mount(flexId, 300, 180);
	StyleSheet::instance().registerRule("row-flex", "display", "flex");
	StyleSheet::instance().registerRule("row-flex", "gap", "10");
	StyleSheet::instance().registerRule("row-flex", "min-height", "40");
	flex.classList().set("row-flex");
	Tree::instance().computeLayout(flexId, 300, 180);

	if (!expectEqual(Tree::instance().node(flexId).style.display, 3, "flex display value")) return 1;
	if (!expectEqual(Tree::instance().node(flexId).layout.height, 40, "flex row min-height")) return 1;
	if (!expectEqual(Tree::instance().node(flexSecondId).layout.x, 60, "flex default row second child x")) return 1;
	if (!expectEqual(Tree::instance().node(flexSecondId).layout.y, 0, "flex default row second child y")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(120, 120, 1.0);

	const int absoluteParentId = Tree::instance().createView();
	const int absoluteChildId = Tree::instance().createView();
	NodeHandle absoluteParent(absoluteParentId);
	absoluteParent.appendChild(NodeHandle(absoluteChildId));
	Tree::instance().mount(absoluteParentId, 120, 120);
	StyleSheet::instance().registerRule("absolute-parent", "position", "relative");
	StyleSheet::instance().registerRule("absolute-parent", "width", "100");
	StyleSheet::instance().registerRule("absolute-parent", "height", "100");
	// This fixture fixes the outer box dimensions, including its padding.
	StyleSheet::instance().registerRule("absolute-parent", "box-sizing", "border-box");
	StyleSheet::instance().registerRule("absolute-parent", "padding", "10");
	StyleSheet::instance().registerRule("absolute-child", "position", "absolute");
	StyleSheet::instance().registerRule("absolute-child", "left", "0");
	StyleSheet::instance().registerRule("absolute-child", "top", "0");
	StyleSheet::instance().registerRule("absolute-child", "bottom", "0");
	StyleSheet::instance().registerRule("absolute-child", "width", "1");
	absoluteParent.classList().set("absolute-parent");
	NodeHandle(absoluteChildId).classList().set("absolute-child");
	Tree::instance().computeLayout(absoluteParentId, 120, 120);

	const auto &absoluteChild = Tree::instance().node(absoluteChildId).layout;
	if (!expectEqual(absoluteChild.x, 0, "absolute child left ignores parent padding")) return 1;
	if (!expectEqual(absoluteChild.y, 0, "absolute child top ignores parent padding")) return 1;
	if (!expectEqual(absoluteChild.height, 100, "absolute child top/bottom spans parent padding box")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(120, 120, 1.0);

	const int outerBlockId = Tree::instance().createView();
	const int containingBlockId = Tree::instance().createView();
	const int staticWrapperId = Tree::instance().createView();
	const int containedAbsoluteId = Tree::instance().createView();
	NodeHandle outerBlock(outerBlockId);
	NodeHandle containingBlock(containingBlockId);
	NodeHandle staticWrapper(staticWrapperId);
	outerBlock.appendChild(containingBlock);
	containingBlock.appendChild(staticWrapper);
	staticWrapper.appendChild(NodeHandle(containedAbsoluteId));
	Tree::instance().mount(outerBlockId, 120, 120);
	StyleSheet::instance().registerRule("outer-block", "width", "120");
	StyleSheet::instance().registerRule("outer-block", "height", "120");
	StyleSheet::instance().registerRule("containing-block", "position", "relative");
	StyleSheet::instance().registerRule("containing-block", "width", "100");
	StyleSheet::instance().registerRule("containing-block", "height", "100");
	StyleSheet::instance().registerRule("containing-block", "margin-left", "10");
	StyleSheet::instance().registerRule("static-wrapper", "width", "30");
	StyleSheet::instance().registerRule("static-wrapper", "height", "20");
	StyleSheet::instance().registerRule("static-wrapper", "margin-top", "40");
	StyleSheet::instance().registerRule("contained-absolute", "position", "absolute");
	StyleSheet::instance().registerRule("contained-absolute", "right", "10");
	StyleSheet::instance().registerRule("contained-absolute", "top", "5");
	StyleSheet::instance().registerRule("contained-absolute", "width", "20");
	StyleSheet::instance().registerRule("contained-absolute", "height", "10");
	outerBlock.classList().set("outer-block");
	containingBlock.classList().set("containing-block");
	staticWrapper.classList().set("static-wrapper");
	NodeHandle(containedAbsoluteId).classList().set("contained-absolute");
	Tree::instance().computeLayout(outerBlockId, 120, 120);

	const auto &containedAbsolute = Tree::instance().node(containedAbsoluteId).layout;
	if (!expectEqual(containedAbsolute.x, 80, "absolute child skips static parent for containing block x")) return 1;
	// The wrapper's top margin collapses with its unpadded containing block.
	// Absolute positioning still uses that block, now at viewport y=40.
	if (!expectEqual(Tree::instance().node(containingBlockId).layout.y, 40, "static wrapper margin collapses with positioned parent")) return 1;
	if (!expectEqual(containedAbsolute.y, 45, "absolute top is relative to shifted containing block")) return 1;
	containingBlock.style().setProperty("padding-top", "1px");
	Tree::instance().computeLayout(outerBlockId, 120, 120);
	if (!expectEqual(Tree::instance().node(staticWrapperId).layout.y, 41, "padding prevents wrapper top margin collapsing")) return 1;
	if (!expectEqual(containedAbsolute.y, 5, "absolute top skips static wrapper even when margin does not collapse")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(120, 120, 1.0);

	const int cardId = Tree::instance().createView();
	const int labelId = Tree::instance().createView();
	const int iconId = Tree::instance().createView();
	const int tempId = Tree::instance().createView();
	NodeHandle card(cardId);
	NodeHandle(labelId).style().width(20);
	NodeHandle(labelId).style().height(10);
	NodeHandle(iconId).style().width(24);
	NodeHandle(iconId).style().height(20);
	NodeHandle(tempId).style().width(18);
	NodeHandle(tempId).style().height(12);
	card.appendChild(NodeHandle(labelId));
	card.appendChild(NodeHandle(iconId));
	card.appendChild(NodeHandle(tempId));
	Tree::instance().mount(cardId, 120, 120);
	StyleSheet::instance().registerRule("hour-card", "display", "grid");
	StyleSheet::instance().registerRule("hour-card", "width", "64");
	StyleSheet::instance().registerRule("hour-card", "height", "77");
	StyleSheet::instance().registerRule("hour-card", "box-sizing", "border-box");
	StyleSheet::instance().registerRule("hour-card", "padding", "5 1");
	StyleSheet::instance().registerRule("hour-card", "gap", "3");
	StyleSheet::instance().registerRule("hour-card", "align-content", "center");
	StyleSheet::instance().registerRule("hour-card", "justify-items", "center");
	card.classList().set("hour-card");
	Tree::instance().computeLayout(cardId, 120, 120);

	const auto &labelLayout = Tree::instance().node(labelId).layout;
	if (!expectEqual(labelLayout.x, 22, "grid justify-items centers hour label")) return 1;
	if (!expectEqual(labelLayout.y, 14, "grid align-content centers hour stack")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 180, 1.0);

	const int nonFlexId = Tree::instance().createView();
	const int nonFlexFirstId = Tree::instance().createView();
	const int nonFlexSecondId = Tree::instance().createView();
	NodeHandle nonFlex(nonFlexId);
	NodeHandle(nonFlexFirstId).style().width(50);
	NodeHandle(nonFlexFirstId).style().height(20);
	NodeHandle(nonFlexSecondId).style().width(50);
	NodeHandle(nonFlexSecondId).style().height(20);
	nonFlex.appendChild(NodeHandle(nonFlexFirstId));
	nonFlex.appendChild(NodeHandle(nonFlexSecondId));
	Tree::instance().mount(nonFlexId, 300, 180);
	StyleSheet::instance().registerRule("row-direction-only", "flex-direction", "row");
	StyleSheet::instance().registerRule("row-direction-only", "gap", "10");
	nonFlex.classList().set("row-direction-only");
	Tree::instance().computeLayout(nonFlexId, 300, 180);

	if (!expectEqual(Tree::instance().node(nonFlexId).style.display, 0, "non-flex display value")) return 1;
	if (!expectEqual(Tree::instance().node(nonFlexSecondId).layout.x, 0, "flex-direction without flex second child x")) return 1;
	if (!expectEqual(Tree::instance().node(nonFlexSecondId).layout.y, 30, "flex-direction without flex second child y")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 180, 1.0);

	const int percentParentId = Tree::instance().createView();
	const int percentChildId = Tree::instance().createView();
	NodeHandle percentParent(percentParentId);
	percentParent.appendChild(NodeHandle(percentChildId));
	Tree::instance().mount(percentParentId, 300, 180);
	StyleSheet::instance().registerRule("percent-parent", "width", "300");
	StyleSheet::instance().registerRule("percent-parent", "height", "180");
	// This fixture fixes the outer box dimensions, including its padding.
	StyleSheet::instance().registerRule("percent-parent", "box-sizing", "border-box");
	StyleSheet::instance().registerRule("percent-parent", "padding", "20");
	StyleSheet::instance().registerRule("percent-child", "width", "100%");
	StyleSheet::instance().registerRule("percent-child", "height", "100%");
	percentParent.classList().set("percent-parent");
	NodeHandle(percentChildId).classList().set("percent-child");
	Tree::instance().computeLayout(percentParentId, 300, 180);

	const auto &percentChildLayout = Tree::instance().node(percentChildId).layout;
	if (!expectEqual(percentChildLayout.x, 20, "percent child padding x")) return 1;
	if (!expectEqual(percentChildLayout.y, 20, "percent child padding y")) return 1;
	if (!expectEqual(percentChildLayout.width, 260, "percent child content-box width")) return 1;
	if (!expectEqual(percentChildLayout.height, 140, "percent child content-box height")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 180, 1.0);

	const int implicitGridId = Tree::instance().createView();
	const int gridFirstId = Tree::instance().createView();
	const int gridSecondId = Tree::instance().createView();
	NodeHandle implicitGrid(implicitGridId);
	implicitGrid.appendChild(NodeHandle(gridFirstId));
	implicitGrid.appendChild(NodeHandle(gridSecondId));
	Tree::instance().mount(implicitGridId, 300, 180);
	StyleSheet::instance().registerRule("implicit-grid", "display", "inline-grid");
	StyleSheet::instance().registerRule("implicit-grid", "width", "300");
	StyleSheet::instance().registerRule("implicit-grid", "height", "180");
	StyleSheet::instance().registerRule("implicit-grid", "grid-template-rows", "80 100");
	NodeHandle(gridFirstId).style().height(20);
	NodeHandle(gridSecondId).style().height(20);
	implicitGrid.classList().set("implicit-grid");
	Tree::instance().computeLayout(implicitGridId, 300, 180);

	if (!expectEqual(Tree::instance().node(implicitGridId).style.display, 2, "inline-grid display value")) return 1;
	if (!expectEqual(Tree::instance().node(gridFirstId).layout.width, 300, "implicit grid fills available column")) return 1;
	if (!expectEqual(Tree::instance().node(gridSecondId).layout.x, 0, "implicit grid second child x")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(120, 80, 1.0);

	const int gridHostId = Tree::instance().createView();
	const int stretchStageId = Tree::instance().createView();
	const int stageContentId = Tree::instance().createView();
	const int absoluteBadgeId = Tree::instance().createView();
	NodeHandle gridHost(gridHostId);
	NodeHandle stretchStage(stretchStageId);
	gridHost.appendChild(stretchStage);
	stretchStage.appendChild(NodeHandle(stageContentId));
	stretchStage.appendChild(NodeHandle(absoluteBadgeId));
	Tree::instance().mount(gridHostId, 120, 80);
	StyleSheet::instance().registerRule("grid-host", "display", "grid");
	StyleSheet::instance().registerRule("grid-host", "width", "120");
	StyleSheet::instance().registerRule("grid-host", "height", "60");
	StyleSheet::instance().registerRule("stretch-stage", "display", "flex");
	StyleSheet::instance().registerRule("stage-content", "width", "20");
	StyleSheet::instance().registerRule("stage-content", "height", "10");
	StyleSheet::instance().registerRule("absolute-badge", "position", "absolute");
	StyleSheet::instance().registerRule("absolute-badge", "right", "5");
	StyleSheet::instance().registerRule("absolute-badge", "top", "0");
	StyleSheet::instance().registerRule("absolute-badge", "width", "30");
	StyleSheet::instance().registerRule("absolute-badge", "height", "10");
	gridHost.classList().set("grid-host");
	stretchStage.classList().set("stretch-stage");
	NodeHandle(stageContentId).classList().set("stage-content");
	NodeHandle(absoluteBadgeId).classList().set("absolute-badge");
	Tree::instance().computeLayout(gridHostId, 120, 80);
	if (!expectEqual(Tree::instance().node(stretchStageId).layout.width, 120, "grid-stretched stage width")) return 1;
	if (!expectEqual(Tree::instance().node(absoluteBadgeId).layout.x, 85, "absolute child right offset after parent stretch")) return 1;

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(80, 80);
	gea::embedded::ui::setViewportMetrics(80, 80, 1.0);

	const int borderProbeId = Tree::instance().createView();
	NodeHandle borderProbe(borderProbeId);
	StyleSheet::instance().registerRule("side-border-probe", "border-top", "1px solid #ffffff");
	StyleSheet::instance().registerRule("side-border-probe", "border-bottom", "2px solid #ffffff");
	borderProbe.classList().set("side-border-probe");
	auto &borderProbeNode = Tree::instance().node(borderProbeId);
	borderProbeNode.layout.x = 10;
	borderProbeNode.layout.y = 10;
	borderProbeNode.layout.width = 40;
	borderProbeNode.layout.height = 20;
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	ViewRenderer::recordBox(borderProbeNode);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(12, 10)) < 220) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected border-top shorthand to paint the top edge\n");
		return 1;
	}
	if (pixelLuma(displayPixelAt(12, 29)) < 220) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected border-bottom shorthand to paint the bottom edge\n");
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(80, 80);
	gea::embedded::ui::setViewportMetrics(80, 80, 1.0);

	const int translateYProbeId = Tree::instance().createView();
	NodeHandle translateYProbe(translateYProbeId);
	StyleSheet::instance().registerRule("translate-y-probe", "background", "#ffffff");
	StyleSheet::instance().registerRule("translate-y-probe", "transform", "translateY(-50%)");
	translateYProbe.classList().set("translate-y-probe");
	auto &translateYNode = Tree::instance().node(translateYProbeId);
	translateYNode.layout.x = 10;
	translateYNode.layout.y = 30;
	translateYNode.layout.width = 40;
	translateYNode.layout.height = 20;
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	ViewRenderer::recordBox(translateYNode);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(12, 22)) < 220) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected translateY(-50%%) to move by half the element height\n");
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(200, 120);
	gea::embedded::ui::setViewportMetrics(200, 120, 1.0);

	const int relativeRootId = Tree::instance().createView();
	const int relativeParentId = Tree::instance().createView();
	const int relativeChildId = Tree::instance().createView();
	NodeHandle relativeRoot(relativeRootId);
	NodeHandle relativeParent(relativeParentId);
	relativeRoot.appendChild(relativeParent);
	relativeParent.appendChild(NodeHandle(relativeChildId));
	Tree::instance().mount(relativeRootId, 120, 80);
	StyleSheet::instance().registerRule("relative-percent-root", "display", "grid");
	StyleSheet::instance().registerRule("relative-percent-root", "width", "120");
	StyleSheet::instance().registerRule("relative-percent-root", "height", "80");
	StyleSheet::instance().registerRule("relative-percent-root", "grid-template-rows", "80");
	StyleSheet::instance().registerRule("relative-percent-child", "position", "relative");
	StyleSheet::instance().registerRule("relative-percent-child", "top", "50%");
	StyleSheet::instance().registerRule("relative-percent-child", "width", "40");
	StyleSheet::instance().registerRule("relative-percent-child", "height", "20");
	StyleSheet::instance().registerRule("relative-percent-child", "background", "#ffffff");
	StyleSheet::instance().registerRule("relative-percent-child", "transform", "translateY(-50%)");
	relativeRoot.classList().set("relative-percent-root");
	NodeHandle(relativeChildId).classList().set("relative-percent-child");
	Tree::instance().computeLayout(relativeRootId, 120, 80);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	DisplayList::instance().recordNode(relativeRootId, 255);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(12, 32)) < 220) {
		const auto &relativeChild = Tree::instance().node(relativeChildId);
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected relative top percent to resolve against parent layout height, child=(%d,%d %dx%d) top=%d topPercent=%d luma32=%d luma52=%d\n",
		             relativeChild.layout.x,
		             relativeChild.layout.y,
		             relativeChild.layout.width,
		             relativeChild.layout.height,
		             relativeChild.style.pos_offsets[0],
		             relativeChild.style.pos_offset_percent[0],
		             pixelLuma(displayPixelAt(12, 32)),
		             pixelLuma(displayPixelAt(12, 52)));
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(120, 100);
	gea::embedded::ui::setViewportMetrics(120, 100, 1.0);

	const int gridItemRootId = Tree::instance().createView();
	const int gridItemId = Tree::instance().createView();
	NodeHandle(gridItemRootId).appendChild(NodeHandle(gridItemId));
	Tree::instance().mount(gridItemRootId, 120, 100);
	StyleSheet::instance().registerRule("relative-grid-root", "display", "grid");
	StyleSheet::instance().registerRule("relative-grid-root", "width", "120");
	StyleSheet::instance().registerRule("relative-grid-root", "height", "100");
	StyleSheet::instance().registerRule("relative-grid-root", "grid-template-rows", "40 60");
	StyleSheet::instance().registerRule("relative-grid-item", "position", "relative");
	StyleSheet::instance().registerRule("relative-grid-item", "top", "50%");
	StyleSheet::instance().registerRule("relative-grid-item", "width", "20");
	StyleSheet::instance().registerRule("relative-grid-item", "background", "#ffffff");
	NodeHandle(gridItemRootId).classList().set("relative-grid-root");
	NodeHandle(gridItemId).classList().set("relative-grid-item");
	Tree::instance().computeLayout(gridItemRootId, 120, 100);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	DisplayList::instance().recordNode(gridItemRootId, 255);
	DisplayList::instance().replay();
	if (Tree::instance().node(gridItemId).layout.y != 20 || pixelLuma(displayPixelAt(2, 22)) < 220) {
		const auto &gridItem = Tree::instance().node(gridItemId);
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected relative top percent on a grid item to resolve against its grid area, item=(%d,%d %dx%d) top=%d topPercent=%d luma22=%d luma52=%d\n",
		             gridItem.layout.x,
		             gridItem.layout.y,
		             gridItem.layout.width,
		             gridItem.layout.height,
		             gridItem.style.pos_offsets[0],
		             gridItem.style.pos_offset_percent[0],
		             pixelLuma(displayPixelAt(2, 22)),
		             pixelLuma(displayPixelAt(2, 52)));
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(120, 100);
	gea::embedded::ui::setViewportMetrics(120, 100, 1.0);

	const int transformRootId = Tree::instance().createView();
	const int transformParentId = Tree::instance().createView();
	const int transformChildId = Tree::instance().createView();
	NodeHandle(transformRootId).appendChild(NodeHandle(transformParentId));
	NodeHandle(transformParentId).appendChild(NodeHandle(transformChildId));
	Tree::instance().mount(transformRootId, 120, 100);
	StyleSheet::instance().registerRule("ancestor-transform-root", "width", "120");
	StyleSheet::instance().registerRule("ancestor-transform-root", "height", "100");
	StyleSheet::instance().registerRule("ancestor-transform-parent", "width", "80");
	StyleSheet::instance().registerRule("ancestor-transform-parent", "height", "40");
	StyleSheet::instance().registerRule("ancestor-transform-parent", "margin-top", "30");
	StyleSheet::instance().registerRule("ancestor-transform-parent", "transform", "translateY(-50%)");
	StyleSheet::instance().registerRule("ancestor-transform-child", "width", "20");
	StyleSheet::instance().registerRule("ancestor-transform-child", "height", "10");
	StyleSheet::instance().registerRule("ancestor-transform-child", "background", "#ffffff");
	NodeHandle(transformRootId).classList().set("ancestor-transform-root");
	NodeHandle(transformParentId).classList().set("ancestor-transform-parent");
	NodeHandle(transformChildId).classList().set("ancestor-transform-child");
	Tree::instance().computeLayout(transformRootId, 120, 100);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	DisplayList::instance().recordNode(transformRootId, 255);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(2, 12)) < 220 || pixelLuma(displayPixelAt(2, 32)) > 32) {
		const auto &parent = Tree::instance().node(transformParentId);
		const auto &child = Tree::instance().node(transformChildId);
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected descendant view to inherit ancestor transform, parent=(%d,%d %dx%d) child=(%d,%d %dx%d) luma12=%d luma32=%d\n",
		             parent.layout.x,
		             parent.layout.y,
		             parent.layout.width,
		             parent.layout.height,
		             child.layout.x,
		             child.layout.y,
		             child.layout.width,
		             child.layout.height,
		             pixelLuma(displayPixelAt(2, 12)),
		             pixelLuma(displayPixelAt(2, 32)));
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(120, 100);
	gea::embedded::ui::setViewportMetrics(120, 100, 1.0);

	std::uint16_t imagePixels[100];
	for (std::uint16_t &pixel : imagePixels) pixel = gea::framework::graphics::pixel::fromRgb888(255, 255, 255);
	const int registeredImageId = gea::framework::graphics::ImageStore::instance().registerBuffer(imagePixels, 10, 10);
	const int imageRootId = Tree::instance().createView();
	const int imageParentId = Tree::instance().createView();
	const int transformedImageId = Tree::instance().createImage();
	NodeHandle(imageRootId).appendChild(NodeHandle(imageParentId));
	NodeHandle(imageParentId).appendChild(NodeHandle(transformedImageId));
	Tree::instance().mount(imageRootId, 120, 100);
	StyleSheet::instance().registerRule("ancestor-image-root", "width", "120");
	StyleSheet::instance().registerRule("ancestor-image-root", "height", "100");
	StyleSheet::instance().registerRule("ancestor-image-parent", "width", "40");
	StyleSheet::instance().registerRule("ancestor-image-parent", "height", "20");
	StyleSheet::instance().registerRule("ancestor-image-parent", "margin-top", "30");
	StyleSheet::instance().registerRule("ancestor-image-parent", "transform", "translateY(-50%)");
	NodeHandle(imageRootId).classList().set("ancestor-image-root");
	NodeHandle(imageParentId).classList().set("ancestor-image-parent");
	Tree::instance().node(transformedImageId).image_id = registeredImageId;
	Tree::instance().computeLayout(imageRootId, 120, 100);
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	DisplayList::instance().recordNode(imageRootId, 255);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(2, 22)) < 220 || pixelLuma(displayPixelAt(2, 32)) > 32) {
		const auto &imageParent = Tree::instance().node(imageParentId);
		const auto &imageNode = Tree::instance().node(transformedImageId);
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected image to inherit ancestor transform, parent=(%d,%d %dx%d) image=(%d,%d %dx%d) luma22=%d luma32=%d\n",
		             imageParent.layout.x,
		             imageParent.layout.y,
		             imageParent.layout.width,
		             imageParent.layout.height,
		             imageNode.layout.x,
		             imageNode.layout.y,
		             imageNode.layout.width,
		             imageNode.layout.height,
		             pixelLuma(displayPixelAt(2, 22)),
		             pixelLuma(displayPixelAt(2, 32)));
		return 1;
	}

	// CSS object-fit must reach ImageFit and preserve a square image inside a
	// rectangular 24x20 display box. The keyed JSX list path also emits the
	// equivalent `fit` attribute, so exercise that generic attribute bridge too.
	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(40, 30);
	gea::embedded::ui::setViewportMetrics(40, 30, 1.0);

	const int fitRootId = Tree::instance().createView();
	const int fitImageId = Tree::instance().createImage();
	NodeHandle(fitRootId).appendChild(NodeHandle(fitImageId));
	Tree::instance().mount(fitRootId, 40, 30);
	StyleSheet::instance().registerRule("object-fit-root", "width", "40");
	StyleSheet::instance().registerRule("object-fit-root", "height", "30");
	StyleSheet::instance().registerRule("object-fit-probe", "width", "24");
	StyleSheet::instance().registerRule("object-fit-probe", "height", "20");
	StyleSheet::instance().registerRule("object-fit-probe", "object-fit", "contain");
	NodeHandle(fitRootId).classList().set("object-fit-root");
	NodeHandle(fitImageId).classList().set("object-fit-probe");
	Tree::instance().node(fitImageId).image_id = registeredImageId;
	Tree::instance().computeLayout(fitRootId, 40, 30);
	if (Tree::instance().node(fitImageId).style.image_fit != 1) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected CSS object-fit:contain to set image_fit=1, got %d\n",
		             Tree::instance().node(fitImageId).style.image_fit);
		return 1;
	}
	NodeHandle(fitImageId).setAttribute("fit", "cover");
	if (Tree::instance().node(fitImageId).style.image_fit != 2) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected fit=cover attribute to set image_fit=2\n");
		return 1;
	}
	NodeHandle(fitImageId).setAttribute("fit", "contain");
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	DisplayList::instance().recordNode(fitRootId, 255);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(0, 10)) > 32 || pixelLuma(displayPixelAt(2, 10)) < 220 ||
	    pixelLuma(displayPixelAt(21, 10)) < 220 || pixelLuma(displayPixelAt(23, 10)) > 32) {
		std::fprintf(stderr,
		             "[test_style_viewport_metrics] expected contain letterboxing in 24x20 box, luma=[%d,%d,%d,%d]\n",
		             pixelLuma(displayPixelAt(0, 10)), pixelLuma(displayPixelAt(2, 10)),
		             pixelLuma(displayPixelAt(21, 10)), pixelLuma(displayPixelAt(23, 10)));
		return 1;
	}

	Tree::instance().clear();
	StyleSheet::instance().clear();
	setNativeDisplaySize(80, 80);
	gea::embedded::ui::setViewportMetrics(80, 80, 1.0);

	const int translateXProbeId = Tree::instance().createView();
	NodeHandle translateXProbe(translateXProbeId);
	StyleSheet::instance().registerRule("translate-x-probe", "background", "#ffffff");
	StyleSheet::instance().registerRule("translate-x-probe", "transform", "translateX(-50%)");
	translateXProbe.classList().set("translate-x-probe");
	auto &translateXNode = Tree::instance().node(translateXProbeId);
	translateXNode.layout.x = 30;
	translateXNode.layout.y = 20;
	translateXNode.layout.width = 40;
	translateXNode.layout.height = 20;
	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	ViewRenderer::recordBox(translateXNode);
	DisplayList::instance().replay();
	if (pixelLuma(displayPixelAt(32, 22)) < 220) {
		std::fprintf(stderr, "[test_style_viewport_metrics] expected translateX(-50%%) to move by half the element width\n");
		return 1;
	}

	return 0;
}
