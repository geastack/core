// SPDX-License-Identifier: Apache-2.0
#include "display_invalidation.h"
#include "tree_state.h"

namespace gea::embedded::ui {

bool DisplayInvalidation::retainsDisplayCommands(Property prop)
{
	switch (prop) {
	case Property::Top:
	case Property::Right:
	case Property::Bottom:
	case Property::Left:
	case Property::TopPercent:
	case Property::RightPercent:
	case Property::BottomPercent:
	case Property::LeftPercent:
		// Position changes are handled by translateNodeCommands (render.cpp)
		// which shifts each command's bbox by (dx, dy) without rebuilding.
		return true;
	default:
		// Opacity is intentionally NOT retained here. render.cpp only emits a
		// SetAlpha command when style.opacity < 255, so a node recorded at
		// opacity=255 has NO SetAlpha command and an in-place "retain" patch
		// can't toggle its visibility. Opacity is instead routed through
		// rebuildsNodeDisplayCommands(): a *leaf* toggling strictly between 0
		// and 255 (e.g. a destroyed brick in tilt-breakout, a hidden
		// button-tetris block) re-records just that node and stays on the
		// direct-replay fast path. Partial opacity or non-leaf nodes fall
		// through to markDisplayListDirty for a correct full rebuild — see the
		// value/leaf gate in tree_style.cpp. (A future patch-SetAlpha-in-place
		// optimization could keep per-frame fade animations on the fast path
		// too.)
		return false;
	}
}

bool DisplayInvalidation::rebuildsNodeDisplayCommands(Property prop)
{
	switch (prop) {
	case Property::Display:
	case Property::Opacity:
	case Property::BackgroundColor:
	case Property::BackgroundAlpha:
	case Property::BackgroundClip:
	case Property::BackgroundImage:
	case Property::Containment:
	case Property::ActiveBackgroundColor:
	case Property::Color:
	case Property::ColorAlpha:
	case Property::BorderAlpha:
	case Property::BorderTopAlpha:
	case Property::BorderRightAlpha:
	case Property::BorderBottomAlpha:
	case Property::BorderLeftAlpha:
	case Property::BorderColorCurrent:
	case Property::BorderTopColorCurrent:
	case Property::BorderRightColorCurrent:
	case Property::BorderBottomColorCurrent:
	case Property::BorderLeftColorCurrent:
	case Property::BorderColor:
	case Property::BorderTopWidth:
	case Property::BorderRightWidth:
	case Property::BorderBottomWidth:
	case Property::BorderLeftWidth:
	case Property::BorderTopColor:
	case Property::BorderRightColor:
	case Property::BorderBottomColor:
	case Property::BorderLeftColor:
	case Property::BorderRadiusTopLeft:
	case Property::BorderRadiusTopRight:
	case Property::BorderRadiusBottomRight:
	case Property::BorderRadiusBottomLeft:
	case Property::BorderRadiusTopLeftPercent:
	case Property::BorderRadiusTopRightPercent:
	case Property::BorderRadiusBottomRightPercent:
	case Property::BorderRadiusBottomLeftPercent:
	case Property::TransformRotate:
	case Property::TransformRotateX:
	case Property::TransformRotateY:
	case Property::TranslateX:
	case Property::RotateAngle:
	case Property::RotateAxisX:
	case Property::RotateAxisY:
	case Property::RotateAxisZ:
	case Property::ScaleX:
	case Property::ScaleY:
	case Property::ScaleZ:
	case Property::TransformTranslateOuterAxes:
	case Property::TranslateY:
	case Property::TranslateZ:
	case Property::TranslateXPercent:
	case Property::TranslateYPercent:
	case Property::TransformTranslateX:
	case Property::TransformTranslateY:
	case Property::TransformTranslateZ:
	case Property::TransformTranslateXPercent:
	case Property::TransformTranslateYPercent:
	case Property::TransformScaleX:
	case Property::TransformScaleY:
	case Property::TransformScaleZ:
	case Property::TransformOriginX:
	case Property::TransformOriginY:
	case Property::Perspective:
	case Property::PerspectiveOriginX:
	case Property::PerspectiveOriginY:
	case Property::FilterBlur:
	case Property::MaskRightFadeWidth:
	case Property::BoxShadowInset:
	case Property::BoxShadowOffsetX:
	case Property::BoxShadowOffsetY:
	case Property::BoxShadowBlur:
	case Property::BoxShadowSpread:
	case Property::BoxShadowColor:
	case Property::BoxShadowAlpha:
		return true;
	default:
		return false;
	}
}

bool DisplayInvalidation::nodeDisplayChangeCanStayLocal(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	Node *n = &state.nodes[node];
	return n->first_child < 0 && n->type != NodeType::Text;
}

}  // namespace gea::embedded::ui
