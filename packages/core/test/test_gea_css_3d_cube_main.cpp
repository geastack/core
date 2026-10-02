#include "native_test_harness.h"
#include "display.h"
#include "pixel.h"
#include "ui/refresh_perf.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdint>

extern void __gea_top_level();

namespace {

bool expectRange(int actual, int minValue, int maxValue, const char *label)
{
	if (actual >= minValue && actual <= maxValue) return true;
	std::fprintf(stderr, "[test_gea_css_3d_cube_main] %s expected %d..%d, got %d\n", label, minValue, maxValue, actual);
	return false;
}

bool expectOne(const char *className, int *out)
{
	const auto nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.size() == 1) {
		*out = nodes[0];
		return true;
	}
	std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected one .%s node, got %zu\n", className, nodes.size());
	gea::embedded::test::dumpTree("test_gea_css_3d_cube_main");
	return false;
}

bool expectOneMounted(const char *className, int *out)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	int match = -1;
	int count = 0;
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		if (!tree.hasClass(nodeId, className)) continue;
		match = nodeId;
		++count;
	}
	if (count == 1) {
		*out = match;
		return true;
	}
	std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected one mounted .%s node, got %d\n", className, count);
	gea::embedded::test::dumpTree("test_gea_css_3d_cube_main");
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

void dumpNodeDetails(int nodeId, const char *label)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const auto &node = tree.node(nodeId);
	std::fprintf(stderr,
	             "[test_gea_css_3d_cube_main] %s #%d class=\"%s\" style=%dx%d min=%dx%d font=%d fontId=%d layout=(%d,%d %dx%d) tx=%d ty=%d tz=%d rx=%d ry=%d rz=%d scale=%d/%d perspective=%d\n",
	             label,
	             nodeId,
	             tree.className(nodeId).c_str(),
	             node.style.width,
	             node.style.height,
	             node.style.min_width,
	             node.style.min_height,
	             node.style.font_size,
	             node.style.font_id,
	             node.layout.x,
	             node.layout.y,
	             node.layout.width,
	             node.layout.height,
	             rstyle(node.style).transform_translate_x,
	             rstyle(node.style).transform_translate_y,
	             rstyle(node.style).transform_translate_z,
	             rstyle(node.style).transform_rotate_x,
	             rstyle(node.style).transform_rotate_y,
	             rstyle(node.style).transform_rotate,
	             rstyle(node.style).transform_scale_x,
	             rstyle(node.style).transform_scale_y,
	             rstyle(node.style).perspective);
}

bool expectGenericGradientAndAlphaPaint()
{
	using namespace gea::embedded::ui;

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Node gradient{};
	gradient.type = NodeType::View;
	gradient.layout.x = 12;
	gradient.layout.y = 12;
	gradient.layout.width = 90;
	gradient.layout.height = 30;
	gradient.style.has_bg = 1;
	gradient.style.bg_fill = 1;
	rstyleMut(gradient.style).bg_gradient_from_color = 0xf800;
	rstyleMut(gradient.style).bg_gradient_to_color = 0x001f;
	rstyleMut(gradient.style).bg_gradient_from_alpha = 255;
	rstyleMut(gradient.style).bg_gradient_to_alpha = 255;
	rstyleMut(gradient.style).bg_gradient_angle = 900;
	gradient.style.bg_alpha = 255;
	gradient.style.bg_color = 0xf800;
	ViewRenderer::recordBox(gradient);
	DisplayList::instance().replay();

	const auto left = gea::embedded::test::displayPixelAt(18, 24);
	const auto right = gea::embedded::test::displayPixelAt(96, 24);
	if (left == 0 || right == 0 || left == right) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected linear-gradient background to paint distinct colors, got left=0x%04x right=0x%04x\n",
		             left,
		             right);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Node translucent{};
	translucent.type = NodeType::View;
	translucent.layout.x = 12;
	translucent.layout.y = 52;
	translucent.layout.width = 40;
	translucent.layout.height = 30;
	translucent.style.has_bg = 1;
	translucent.style.bg_fill = 0;
	translucent.style.bg_alpha = 128;
	translucent.style.bg_color = 0xf800;
	ViewRenderer::recordBox(translucent);
	DisplayList::instance().replay();

	const auto blended = gea::embedded::test::displayPixelAt(20, 60);
	if (blended == 0 || blended == 0xf800) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected rgba background to blend, got pixel=0x%04x\n",
		             blended);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	Node grid{};
	grid.type = NodeType::View;
	grid.layout.x = 8;
	grid.layout.y = 88;
	grid.layout.width = 48;
	grid.layout.height = 48;
	grid.style.has_bg = 1;
	grid.style.bg_color = 0x0000;
	grid.style.bg_alpha = 255;
	rstyleMut(grid.style).bg_grid_axes = 3;
	rstyleMut(grid.style).bg_grid_color = 0xffff;
	rstyleMut(grid.style).bg_grid_alpha = 160;
	rstyleMut(grid.style).bg_grid_step_x = 12;
	rstyleMut(grid.style).bg_grid_step_y = 12;
	rstyleMut(grid.style).bg_grid_line_x = 1;
	rstyleMut(grid.style).bg_grid_line_y = 1;
	ViewRenderer::recordBox(grid);
	DisplayList::instance().replay();

	const auto verticalLine = gea::embedded::test::displayPixelAt(20, 96);
	const auto horizontalLine = gea::embedded::test::displayPixelAt(24, 100);
	const auto openCell = gea::embedded::test::displayPixelAt(24, 96);
	if (verticalLine == 0 || horizontalLine == 0 || openCell != 0) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected background-size tiled gradient grid, got vertical=0x%04x horizontal=0x%04x open=0x%04x\n",
		             verticalLine,
		             horizontalLine,
		             openCell);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	auto &tree = Tree::instance();
	const int hexColorId = tree.createText();
	StyleSheet::instance().applyProperty(NodeHandle(hexColorId), "color", "#12130fbd");
	const auto &hexColorNode = tree.node(hexColorId);
	const auto expectedHexColor = gea::framework::graphics::pixel::fromRgb888(0x12, 0x13, 0x0f);
	if (hexColorNode.style.text_color != expectedHexColor || hexColorNode.style.text_alpha != 0xbd) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected #rrggbbaa text color to parse rgb=0x%04x alpha=0xbd, got rgb=0x%04x alpha=0x%02x\n",
		             expectedHexColor,
		             hexColorNode.style.text_color,
		             hexColorNode.style.text_alpha);
		return false;
	}

	const int shortHexColorId = tree.createText();
	StyleSheet::instance().applyProperty(NodeHandle(shortHexColorId), "color", "#123b");
	const auto &shortHexColorNode = tree.node(shortHexColorId);
	const auto expectedShortHexColor = gea::framework::graphics::pixel::fromRgb888(0x11, 0x22, 0x33);
	if (shortHexColorNode.style.text_color != expectedShortHexColor || shortHexColorNode.style.text_alpha != 0xbb) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected #rgba text color to parse rgb=0x%04x alpha=0xbb, got rgb=0x%04x alpha=0x%02x\n",
		             expectedShortHexColor,
		             shortHexColorNode.style.text_color,
		             shortHexColorNode.style.text_alpha);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	const int blurId = tree.createView();
	Node &blur = tree.node(blurId);
	blur.layout.x = 8;
	blur.layout.y = 8;
	blur.layout.width = 24;
	blur.layout.height = 24;
	blur.style.has_bg = 1;
	blur.style.bg_alpha = 255;
	blur.style.bg_color = 0xffff;
	rstyleMut(blur.style).filter_blur_radius = 5;
	DisplayList::instance().recordNode(blurId, 255);

	int cacheHits = 0;
	int cacheMisses = 0;
	DisplayList::instance().filterBlurCacheStats(&cacheHits, &cacheMisses);
	DisplayList::instance().replay();
	DisplayList::instance().filterBlurCacheStats(&cacheHits, &cacheMisses);
	if (cacheMisses != 1) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected first blur replay to miss cache once, got hits=%d misses=%d\n",
		             cacheHits,
		             cacheMisses);
		return false;
	}
	const auto outsideBlur = gea::embedded::test::displayPixelAt(5, 20);
	if (outsideBlur == 0) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected filter: blur() to soften outside the source rect\n");
		return false;
	}
	DisplayList::instance().replay();
	DisplayList::instance().filterBlurCacheStats(&cacheHits, &cacheMisses);
	if (cacheHits < 1 || cacheMisses != 1) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected second blur replay to reuse cache, got hits=%d misses=%d\n",
		             cacheHits,
		             cacheMisses);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	const int projectedRootId = tree.createView();
	Node &projectedRoot = tree.node(projectedRootId);
	projectedRoot.layout.x = 0;
	projectedRoot.layout.y = 0;
	projectedRoot.layout.width = 320;
	projectedRoot.layout.height = 220;
	const int projectedBlurId = tree.createView();
	tree.setParent(projectedBlurId, projectedRootId);
	Node &projectedBlur = tree.node(projectedBlurId);
	projectedBlur.layout.x = 80;
	projectedBlur.layout.y = 160;
	projectedBlur.layout.width = 180;
	projectedBlur.layout.height = 34;
	projectedBlur.style.has_bg = 1;
	projectedBlur.style.bg_alpha = 255;
	projectedBlur.style.bg_color = 0xffff;
	projectedBlur.style.border_radius[0] = 90;
	projectedBlur.style.border_radius[1] = 90;
	projectedBlur.style.border_radius[2] = 90;
	projectedBlur.style.border_radius[3] = 90;
	rstyleMut(projectedBlur.style).transform_origin_x = 500;
	rstyleMut(projectedBlur.style).transform_origin_y = 500;
	rstyleMut(projectedBlur.style).transform_rotate_x = 720;
	rstyleMut(projectedBlur.style).filter_blur_radius = 18;
	DisplayList::instance().recordNode(projectedRootId, 255);
	DisplayList::instance().replay();
	const auto projectedBlurFalloff = gea::embedded::test::displayPixelAt(170, 166);
	if (projectedBlurFalloff == 0) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected transformed filter: blur() to stay soft after projection\n");
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	const int blurTailRootId = tree.createView();
	Node &blurTailRoot = tree.node(blurTailRootId);
	blurTailRoot.layout.x = 0;
	blurTailRoot.layout.y = 0;
	blurTailRoot.layout.width = 180;
	blurTailRoot.layout.height = 120;
	blurTailRoot.style.has_bg = 1;
	blurTailRoot.style.bg_alpha = 255;
	blurTailRoot.style.bg_color = 0x2344;
	const int blurTailId = tree.createView();
	tree.setParent(blurTailId, blurTailRootId);
	Node &blurTail = tree.node(blurTailId);
	blurTail.layout.x = 70;
	blurTail.layout.y = 52;
	blurTail.layout.width = 70;
	blurTail.layout.height = 22;
	blurTail.style.has_bg = 1;
	blurTail.style.bg_alpha = 128;
	blurTail.style.bg_color = 0x0000;
	blurTail.style.border_radius[0] = 35;
	blurTail.style.border_radius[1] = 35;
	blurTail.style.border_radius[2] = 35;
	blurTail.style.border_radius[3] = 35;
	rstyleMut(blurTail.style).filter_blur_radius = 10;
	DisplayList::instance().recordNode(blurTailRootId, 255);
	DisplayList::instance().replay();
	const int blurBg = pixelLuma(gea::embedded::test::displayPixelAt(30, 63));
	const int blurCenter = pixelLuma(gea::embedded::test::displayPixelAt(105, 63));
	const int blurOutside = pixelLuma(gea::embedded::test::displayPixelAt(62, 63));
	const int blurFar = pixelLuma(gea::embedded::test::displayPixelAt(48, 63));
	const int blurCenterDarkening = blurBg - blurCenter;
	const int blurOutsideDarkening = blurBg - blurOutside;
	const int blurFarDarkening = blurBg - blurFar;
	if (blurCenterDarkening < 8 ||
	    blurOutsideDarkening <= 0 ||
	    blurOutsideDarkening >= blurCenterDarkening ||
	    blurFarDarkening >= blurOutsideDarkening) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected filter: blur() to extend beyond the source bounds with a soft falloff, got bg=%d center=%d outside=%d far=%d darkening=%d/%d/%d\n",
		             blurBg,
		             blurCenter,
		             blurOutside,
		             blurFar,
		             blurCenterDarkening,
		             blurOutsideDarkening,
		             blurFarDarkening);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();

	const int shadowRootId = tree.createView();
	Node &shadowRoot = tree.node(shadowRootId);
	shadowRoot.layout.x = 0;
	shadowRoot.layout.y = 0;
	shadowRoot.layout.width = 320;
	shadowRoot.layout.height = 220;
	shadowRoot.style.has_bg = 1;
	shadowRoot.style.bg_alpha = 255;
	shadowRoot.style.bg_color = 0x2344;
	const int shadowId = tree.createView();
	tree.setParent(shadowId, shadowRootId);
	Node &shadow = tree.node(shadowId);
	shadow.layout.x = 52;
	shadow.layout.y = 130;
	shadow.layout.width = 216;
	shadow.layout.height = 34;
	shadow.style.opacity = 82;
	shadow.style.has_bg = 1;
	shadow.style.bg_alpha = 108;
	shadow.style.bg_color = 0x0000;
	shadow.style.border_radius[0] = 108;
	shadow.style.border_radius[1] = 108;
	shadow.style.border_radius[2] = 108;
	shadow.style.border_radius[3] = 108;
	rstyleMut(shadow.style).transform_origin_x = 500;
	rstyleMut(shadow.style).transform_origin_y = 500;
	rstyleMut(shadow.style).transform_rotate_x = 720;
	rstyleMut(shadow.style).filter_blur_radius = 18;
	DisplayList::instance().recordNode(shadowRootId, 255);
	DisplayList::instance().replay();
	const int shadowBg = pixelLuma(gea::embedded::test::displayPixelAt(32, 145));
	const int shadowCenter = pixelLuma(gea::embedded::test::displayPixelAt(160, 145));
	const int shadowEdge = pixelLuma(gea::embedded::test::displayPixelAt(56, 145));
	const int shadowOuter = pixelLuma(gea::embedded::test::displayPixelAt(44, 145));
	const int centerDarkening = shadowBg - shadowCenter;
	const int edgeDarkening = shadowBg - shadowEdge;
	const int outerDarkening = shadowBg - shadowOuter;
	if (centerDarkening < 4 ||
	    centerDarkening > 12 ||
	    edgeDarkening > centerDarkening ||
	    outerDarkening > edgeDarkening) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected filtered shadow edge to softly fall off, got bg=%d center=%d edge=%d outer=%d darkening=%d/%d/%d\n",
		             shadowBg,
		             shadowCenter,
		             shadowEdge,
		             shadowOuter,
		             centerDarkening,
		             edgeDarkening,
		             outerDarkening);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	tree.clear();
	DisplayList::instance().clear();

	const int zRootId = tree.createView();
	Node &zRoot = tree.node(zRootId);
	zRoot.layout.x = 0;
	zRoot.layout.y = 0;
	zRoot.layout.width = 220;
	zRoot.layout.height = 180;
	zRoot.style.has_bg = 1;
	zRoot.style.bg_alpha = 255;
	zRoot.style.bg_color = 0x0000;
	const int zWrapId = tree.createView();
	tree.setParent(zWrapId, zRootId);
	Node &zWrap = tree.node(zWrapId);
	zWrap.layout.x = 40;
	zWrap.layout.y = 50;
	zWrap.layout.width = 120;
	zWrap.layout.height = 80;
	const int zFaceId = tree.createView();
	tree.setParent(zFaceId, zWrapId);
	Node &zFace = tree.node(zFaceId);
	zFace.layout.x = 70;
	zFace.layout.y = 78;
	zFace.layout.width = 62;
	zFace.layout.height = 46;
	zFace.style.has_bg = 1;
	zFace.style.bg_alpha = 255;
	zFace.style.bg_color = 0xf800;
	rstyleMut(zFace.style).transform_translate_z = 80;
	// translateZ only reorders paint inside a 3D rendering context (CSS Transforms 2):
	// the root and the wrapper that holds the face both preserve it.
	rstyleMut(zRoot.style).transform_preserve_3d = 1;
	rstyleMut(zWrap.style).transform_preserve_3d = 1;
	const int zShadowId = tree.createView();
	tree.setParent(zShadowId, zRootId);
	Node &zShadow = tree.node(zShadowId);
	zShadow.layout.x = 50;
	zShadow.layout.y = 82;
	zShadow.layout.width = 112;
	zShadow.layout.height = 28;
	zShadow.style.has_bg = 1;
	zShadow.style.bg_alpha = 160;
	zShadow.style.bg_color = 0x0000;
	zShadow.style.border_radius[0] = 56;
	zShadow.style.border_radius[1] = 56;
	zShadow.style.border_radius[2] = 56;
	zShadow.style.border_radius[3] = 56;
	rstyleMut(zShadow.style).filter_blur_radius = 8;
	DisplayList::instance().recordNode(zRootId, 255);
	DisplayList::instance().replay();
	const auto foregroundOverShadow = gea::embedded::test::displayPixelAt(90, 92);
	if (foregroundOverShadow != 0xf800) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected positive-z descendant subtree to paint above later filtered sibling, got pixel=0x%04x\n",
		             foregroundOverShadow);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	const int insetShadowId = tree.createView();
	Node &insetShadow = tree.node(insetShadowId);
	insetShadow.layout.x = 48;
	insetShadow.layout.y = 48;
	insetShadow.layout.width = 96;
	insetShadow.layout.height = 72;
	insetShadow.style.has_bg = 1;
	insetShadow.style.bg_alpha = 255;
	insetShadow.style.bg_color = 0xffff;
	StyleSheet::instance().applyProperty(NodeHandle(insetShadowId), "box-shadow", "inset 0 0 12px rgba(0, 0, 0, 0.5)");
	DisplayList::instance().recordNode(insetShadowId, 255);
	DisplayList::instance().replay();
	const auto insetCenter = gea::embedded::test::displayPixelAt(96, 84);
	const auto insetLeftEdge = gea::embedded::test::displayPixelAt(50, 84);
	if (insetCenter != 0xffff || insetLeftEdge == 0xffff || insetLeftEdge == 0) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected inset box-shadow to darken inside edge only, got center=0x%04x left=0x%04x\n",
		             insetCenter,
		             insetLeftEdge);
		return false;
	}

	gea::platform::display::Display::clearNoFlush();
	DisplayList::instance().clear();
	auto *alpha = DisplayList::instance().append();
	auto *quad = DisplayList::instance().append();
	auto *resetAlpha = DisplayList::instance().append();
	if (!alpha || !quad || !resetAlpha) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] failed to append alpha quad commands\n");
		return false;
	}
	alpha->type = DisplayCommandType::SetAlpha;
	alpha->bx = 40; alpha->by = 40; alpha->bw = 32; alpha->bh = 32;
	alpha->alpha.alpha = 128;
	quad->type = DisplayCommandType::FillQuad;
	quad->bx = 40; quad->by = 40; quad->bw = 32; quad->bh = 32;
	quad->quad.x0 = 40; quad->quad.y0 = 40;
	quad->quad.x1 = 72; quad->quad.y1 = 40;
	quad->quad.x2 = 72; quad->quad.y2 = 72;
	quad->quad.x3 = 40; quad->quad.y3 = 72;
	quad->quad.color = 0xffff;
	resetAlpha->type = DisplayCommandType::SetAlpha;
	resetAlpha->bx = 40; resetAlpha->by = 40; resetAlpha->bw = 32; resetAlpha->bh = 32;
	resetAlpha->alpha.alpha = 255;
	DisplayList::instance().replay();
	const auto onceBlended = gea::embedded::test::displayPixelAt(47, 48);
	const auto diagonalBlended = gea::embedded::test::displayPixelAt(56, 56);
	if (onceBlended != diagonalBlended) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] expected translucent FillQuad to blend each pixel once, got off-diagonal=0x%04x diagonal=0x%04x\n",
		             onceBlended,
		             diagonalBlended);
		return false;
	}

	return true;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	if (!expectGenericGradientAndAlphaPaint()) return 1;

	resetNativeHost();
	__gea_top_level();
	refresh();
	StyleSheet::instance().startCssAnimations(0);
	pumpFrame(0);

	int app = -1;
	int title = -1;
	int kicker = -1;
	int heading = -1;
	int stage = -1;
	int shadow = -1;
	int wrap = -1;
	int cube = -1;
	int face = -1;
	if (!expectOne("cube-app", &app)) return 1;
	if (!expectOne("cube-title", &title)) return 1;
	if (!expectOne("cube-kicker", &kicker)) return 1;
	if (!expectOne("cube-heading", &heading)) return 1;
	if (!expectOne("cube-stage", &stage)) return 1;
	if (!expectOneMounted("cube-shadow", &shadow)) return 1;
	if (!expectOne("cube-wrap", &wrap)) return 1;
	if (!expectOne("cube", &cube)) return 1;
	if (!expectOne("cube-face--front", &face)) return 1;
	auto faceNodes = nodesWithClass("cube-face");
	if (faceNodes.size() != 6) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected six .cube-face nodes, got %zu\n", faceNodes.size());
		return 1;
	}
	const auto labelNodes = nodesWithClass("cube-face-label");
	if (labelNodes.size() != 6) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected six .cube-face-label nodes, got %zu\n", labelNodes.size());
		return 1;
	}

	auto &tree = Tree::instance();
	dumpNodeDetails(app, "app");
	dumpNodeDetails(title, "title");
	dumpNodeDetails(kicker, "kicker");
	dumpNodeDetails(heading, "heading");
	dumpNodeDetails(stage, "stage");
	dumpNodeDetails(shadow, "shadow");
	dumpNodeDetails(wrap, "wrap");
	dumpNodeDetails(cube, "cube");
	dumpNodeDetails(face, "front face");

	if (!expectRange(tree.node(app).layout.width, 408, 410, "app width")) return 1;
	if (!expectRange(tree.node(app).layout.height, 500, 502, "app height")) return 1;
	if (!expectRange(tree.node(heading).style.font_size, 56, 63, "heading font-size")) return 1;
	if (tree.node(heading).style.font_id < 0) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] heading should use generated font family\n");
		return 1;
	}
	if (!expectRange(tree.node(heading).layout.height, 50, 56, "heading rendered font height")) return 1;
	if (tree.node(kicker).style.text_transform != 1) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube kicker should parse text-transform: uppercase, got %d\n",
		             tree.node(kicker).style.text_transform);
		return 1;
	}
	if (!expectRange(tree.node(stage).layout.width, 318, 321, "stage width")) return 1;
	if (!expectRange(tree.node(stage).layout.height, 318, 321, "stage height")) return 1;
	if (rstyle(tree.node(stage).style).transform_scale_x != 1000 ||
	    rstyle(tree.node(stage).style).transform_scale_y != 1000) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube stage should not scale with the cube, got scale=%d/%d\n",
		             rstyle(tree.node(stage).style).transform_scale_x,
		             rstyle(tree.node(stage).style).transform_scale_y);
		return 1;
	}
	const int stageCenterX = tree.node(stage).layout.x + tree.node(stage).layout.width / 2;
	const int stageCenterY = tree.node(stage).layout.y + tree.node(stage).layout.height / 2;
	if (tree.node(shadow).style.display != 1) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube shadow should be disabled for ESP32 perf triage, got display=%d\n",
		             tree.node(shadow).style.display);
		return 1;
	}
	if (rstyle(tree.node(shadow).style).filter_blur_radius != 0) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] disabled cube shadow should not keep filter: blur(), got %d\n",
		             rstyle(tree.node(shadow).style).filter_blur_radius);
		return 1;
	}
	if (!expectRange(tree.node(wrap).layout.width, 154, 155, "wrap width")) return 1;
	if (!expectRange(tree.node(wrap).layout.height, 154, 155, "wrap height")) return 1;
	if (rstyle(tree.node(wrap).style).transform_scale_x != 490 ||
	    rstyle(tree.node(wrap).style).transform_scale_y != 490) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube wrapper should scale the cube itself to 1/2 size, got scale=%d/%d\n",
		             rstyle(tree.node(wrap).style).transform_scale_x,
		             rstyle(tree.node(wrap).style).transform_scale_y);
		return 1;
	}
	const int wrapCenterX = tree.node(wrap).layout.x + tree.node(wrap).layout.width / 2;
	const int wrapCenterY = tree.node(wrap).layout.y + tree.node(wrap).layout.height / 2;
	if (!expectRange(wrapCenterX, stageCenterX - 1, stageCenterX + 1, "wrap centered x")) return 1;
	if (!expectRange(wrapCenterY, stageCenterY - 1, stageCenterY + 1, "wrap centered y")) return 1;
	if (!expectRange(tree.node(face).layout.width, 154, 155, "front face width")) return 1;
	if (!expectRange(tree.node(face).layout.height, 154, 155, "front face height")) return 1;
	if (!expectRange(rstyle(tree.node(face).style).transform_translate_z, 77, 78, "front translateZ")) return 1;
	if (rstyle(tree.node(face).style).box_shadow_inset != 0 ||
	    rstyle(tree.node(face).style).box_shadow_blur_radius != 0 ||
	    rstyle(tree.node(face).style).box_shadow_alpha != 0) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube face shadows should be disabled for ESP32 perf triage, got inset=%d blur=%d alpha=%d color=0x%04x\n",
		             rstyle(tree.node(face).style).box_shadow_inset,
		             rstyle(tree.node(face).style).box_shadow_blur_radius,
		             rstyle(tree.node(face).style).box_shadow_alpha,
		             rstyle(tree.node(face).style).box_shadow_color);
		return 1;
	}
	const auto expectedLabelColor = gea::framework::graphics::pixel::fromRgb888(0x12, 0x13, 0x0f);
	if (!tree.hasClass(cube, "cube--opaque")) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] cube should start in opaque mode\n");
		return 1;
	}
	for (int faceNode : faceNodes) {
		if (tree.node(faceNode).style.backface_hidden != 1) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] opaque face should have backface hidden, node=%d value=%d\n",
			             faceNode,
			             tree.node(faceNode).style.backface_hidden);
			return 1;
		}
	}
	for (int label : labelNodes) {
		if (tree.node(label).style.text_color != expectedLabelColor || tree.node(label).style.text_alpha != 0xbd) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] face label should use #12130fbd, got rgb=0x%04x alpha=0x%02x\n",
			             tree.node(label).style.text_color,
			             tree.node(label).style.text_alpha);
			return 1;
		}
		if (tree.node(label).style.text_transform != 1) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] face label should parse text-transform: uppercase, got %d\n",
			             tree.node(label).style.text_transform);
			return 1;
		}
		if (tree.node(label).style.backface_hidden != 0) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] backface-visibility should not inherit onto label node=%d value=%d\n",
			             label,
			             tree.node(label).style.backface_hidden);
			return 1;
		}
		const int parent = tree.node(label).parent;
		const int labelCenterX = tree.node(label).layout.x + tree.node(label).layout.width / 2;
		const int labelCenterY = tree.node(label).layout.y + tree.node(label).layout.height / 2;
		const int parentCenterX = tree.node(parent).layout.x + tree.node(parent).layout.width / 2;
		const int parentCenterY = tree.node(parent).layout.y + tree.node(parent).layout.height / 2;
		if (!expectRange(labelCenterX, parentCenterX - 2, parentCenterX + 2, "face label centered x")) return 1;
		if (!expectRange(labelCenterY, parentCenterY - 2, parentCenterY + 2, "face label centered y")) return 1;
	}

	if (!dispatchClick(app)) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected root click to toggle cube opacity\n");
		return 1;
	}
	refresh();
	if (!expectOne("cube", &cube)) return 1;
	if (tree.hasClass(cube, "cube--opaque")) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] root click should remove .cube--opaque\n");
		return 1;
	}
	faceNodes = nodesWithClass("cube-face");
	for (int faceNode : faceNodes) {
		if (tree.node(faceNode).style.backface_hidden != 0) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] translucent face should make backface visible, node=%d value=%d class=\"%s\"\n",
			             faceNode,
			             tree.node(faceNode).style.backface_hidden,
			             tree.className(faceNode).c_str());
			return 1;
		}
	}
	for (int label : nodesWithClass("cube-face-label")) {
		bool sawProjectedText = false;
		for (int i = 0; i < DisplayList::instance().nodeCommandCount(label); ++i) {
			const DisplayCommand *command = DisplayList::instance().nodeCommandAt(label, i);
			if (!command || command->type != DisplayCommandType::DrawProjectedText) continue;
			sawProjectedText = true;
			if (command->projectedText.backfaceHidden != 0) {
				std::fprintf(stderr,
				             "[test_gea_css_3d_cube_main] translucent projected label should not be backface-culled, node=%d text=\"%s\"\n",
				             label,
				             tree.node(label).text.c_str());
				return 1;
			}
		}
		if (!sawProjectedText) {
			std::fprintf(stderr,
			             "[test_gea_css_3d_cube_main] cube face label should emit projected text command, node=%d text=\"%s\"\n",
			             label,
			             tree.node(label).text.c_str());
			return 1;
		}
	}
	if (!dispatchClick(app)) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] expected second root click to restore opaque mode\n");
		return 1;
	}
	refresh();
	if (!expectOne("cube", &cube)) return 1;
	if (!tree.hasClass(cube, "cube--opaque")) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] second root click should restore .cube--opaque\n");
		return 1;
	}

	const auto &cubeInitial = tree.node(cube);
	if (rstyle(cubeInitial.style).transform_rotate_x != -180 ||
	    rstyle(cubeInitial.style).transform_rotate_y != 240) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] cube animation should apply first keyframe, got rotateX=%d rotateY=%d\n",
		             rstyle(cubeInitial.style).transform_rotate_x,
		             rstyle(cubeInitial.style).transform_rotate_y);
		return 1;
	}

	gea::platform::display::Display::flushStatsReset();
	refreshPerfStatsReset();
	pumpFrame(1000);
	const auto &cubeAfter = tree.node(cube);
	if (rstyle(cubeAfter.style).transform_rotate_x == -180 && rstyle(cubeAfter.style).transform_rotate_y == 240) {
		std::fprintf(stderr, "[test_gea_css_3d_cube_main] cube animation did not advance\n");
		return 1;
	}
	const int animatedFlushPixels = flushPixelCount();
	const int fullViewportPixels = tree.mountedWidth() * tree.mountedHeight();
	if (animatedFlushPixels >= (fullViewportPixels * 3) / 4) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] animated non-leaf transform should not repaint most of the viewport, flushed=%d full=%d\n",
		             animatedFlushPixels,
		             fullViewportPixels);
		return 1;
	}
	const auto animatedRefreshPerf = refreshPerfStatsRead();
	if (animatedRefreshPerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_gea_css_3d_cube_main] animated non-leaf transform should use direct dirty replay, direct=%d refreshes=%d dlist_us=%lld replay_us=%lld\n",
		             animatedRefreshPerf.treeDirectReplayCalls,
		             animatedRefreshPerf.treeRefreshCalls,
		             static_cast<long long>(animatedRefreshPerf.treeDisplayListUs),
		             static_cast<long long>(animatedRefreshPerf.treeReplayUs));
		return 1;
	}

	return 0;
}
