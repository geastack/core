// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "image.h"

#include <image.h>

namespace gea::embedded::ui {

namespace {

struct ImageFitRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

class ImageFitResolver {
public:
	static ImageFitRect resolve(const Node &node, int imageWidth, int imageHeight)
	{
		const int contentWidth = node.layout.width - boxInsets(node.style, true);
		const int contentHeight = node.layout.height - boxInsets(node.style, false);
		ImageFitRect rect{
			node.layout.x + boxInset(node.style, 3),
			node.layout.y + boxInset(node.style, 0),
			contentWidth,
			contentHeight,
		};

		if (node.style.image_fit == 1 || node.style.image_fit == 4) {
			const int scaledWidth = (imageWidth * contentHeight) / imageHeight;
			const int scaledHeight = (imageHeight * contentWidth) / imageWidth;
			if (scaledWidth <= contentWidth) {
				rect.width = scaledWidth;
				rect.height = contentHeight;
			} else {
				rect.width = contentWidth;
				rect.height = scaledHeight;
			}
			if (node.style.image_fit == 4 && rect.width >= imageWidth && rect.height >= imageHeight) {
				rect.width = imageWidth;
				rect.height = imageHeight;
			}
			center(node, rect);
		} else if (node.style.image_fit == 2) {
			const int scaledWidth = (imageWidth * contentHeight) / imageHeight;
			const int scaledHeight = (imageHeight * contentWidth) / imageWidth;
			if (scaledWidth >= contentWidth) {
				rect.width = scaledWidth;
				rect.height = contentHeight;
			} else {
				rect.width = contentWidth;
				rect.height = scaledHeight;
			}
			center(node, rect);
		} else if (node.style.image_fit == 3) {
			rect.width = imageWidth;
			rect.height = imageHeight;
			center(node, rect);
		}

		return rect;
	}

private:
	static void center(const Node &node, ImageFitRect &rect)
	{
		rect.x = node.layout.x + boxInset(node.style, 3) + (node.layout.width - boxInsets(node.style, true) - rect.width) / 2;
		rect.y = node.layout.y + boxInset(node.style, 0) + (node.layout.height - boxInsets(node.style, false) - rect.height) / 2;
	}
};

bool hasImageRadius(const Node &node)
{
	return node.style.border_radius[0] > 0 ||
	       node.style.border_radius[1] > 0 ||
	       node.style.border_radius[2] > 0 ||
	       node.style.border_radius[3] > 0;
}

void setScaledBlitRadius(DisplayCommand *cmd, const Node &node)
{
	cmd->scaledBlit.tl = node.style.border_radius[0];
	cmd->scaledBlit.tr = node.style.border_radius[1];
	cmd->scaledBlit.br = node.style.border_radius[2];
	cmd->scaledBlit.bl = node.style.border_radius[3];
}

bool applyAxisAlignedTransform(const Node &node, ImageFitRect &rect)
{
	int16_t xs[4] = {};
	int16_t ys[4] = {};
	ViewRenderer::transformedRectCorners(node, false, rect.x, rect.y, rect.width, rect.height, xs, ys);
	if (xs[0] != xs[3] || xs[1] != xs[2] || ys[0] != ys[1] || ys[2] != ys[3]) return false;
	if (xs[1] < xs[0] || ys[2] < ys[0]) return false;

	rect.x = xs[0];
	rect.y = ys[0];
	rect.width = xs[1] - xs[0];
	rect.height = ys[2] - ys[0];
	return rect.width > 0 && rect.height > 0;
}

}  // namespace

void ImageRenderer::layout(int id)
{
	Node *n = &Tree::instance().nodes()[id];
	auto &images = gea::framework::graphics::ImageStore::instance();
	int iw = images.width(n->image_id);
	int ih = images.height(n->image_id);

	if (n->style.width == kUnset && n->style.width_percent == kUnset && n->style.width_expression < 0 && iw > 0)
		n->layout.width = iw + boxInsets(n->style, true);
	if (n->style.height == kUnset && n->style.height_percent == kUnset && n->style.height_expression < 0 && ih > 0)
		n->layout.height = ih + boxInsets(n->style, false);
	n->layout.width = clampBorderBoxSize(n->style, n->layout.width, true);
	n->layout.height = clampBorderBoxSize(n->style, n->layout.height, false);
}

void ImageRenderer::record(const Node &node)
{
	const Node *n = &node;
	if (n->type != NodeType::Image || n->image_id < 0) return;

	auto &images = gea::framework::graphics::ImageStore::instance();
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(n->image_id);
	if (!pixels) return;
	const uint8_t *alpha = images.currentAlpha(n->image_id);

	int iw = images.width(n->image_id);
	int ih = images.height(n->image_id);
	if (iw <= 0 || ih <= 0) return;

	ImageFitRect fit = ImageFitResolver::resolve(*n, iw, ih);
	applyAxisAlignedTransform(*n, fit);

	if (iw == fit.width && ih == fit.height && !hasImageRadius(*n)) {
		DisplayCommand *cmd = DisplayList::instance().append();
		if (!cmd) return;
		cmd->type = DisplayCommandType::BlitImage;
		cmd->bx = fit.x; cmd->by = fit.y; cmd->bw = fit.width; cmd->bh = fit.height;
		cmd->blit.pixels = pixels;
		cmd->blit.alpha = alpha;
		cmd->blit.sourceWidth = iw;
		cmd->blit.sourceHeight = ih;
		cmd->blit.dx = fit.x;
		cmd->blit.dy = fit.y;
		cmd->blit.sourcePacked = 0;  // decoded images are unpacked (one value per byte)
		return;
	}

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::BlitImageScaled;
	cmd->bx = fit.x; cmd->by = fit.y; cmd->bw = fit.width; cmd->bh = fit.height;
	cmd->scaledBlit.pixels = pixels;
	cmd->scaledBlit.alpha = alpha;
	cmd->scaledBlit.sourceWidth = iw;
	cmd->scaledBlit.sourceHeight = ih;
	cmd->scaledBlit.dx = fit.x;
	cmd->scaledBlit.dy = fit.y;
	cmd->scaledBlit.dw = fit.width;
	cmd->scaledBlit.dh = fit.height;
	setScaledBlitRadius(cmd, *n);
}

}  // namespace gea::embedded::ui
