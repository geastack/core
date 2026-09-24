#include "native_test_harness.h"

#include "css/engine.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>

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
	std::fprintf(stderr, "[test_css_animation_priming] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

bool expectTrue(bool value, const char *label)
{
	if (value) return true;
	std::fprintf(stderr, "[test_css_animation_priming] %s failed\n", label);
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	StyleSheet::instance().clear();

	StyleSheet::instance().registerKeyframeRule("cube-spin", 0, "transform", "rotateX(-18deg) rotateY(24deg) rotateZ(0deg)");
	StyleSheet::instance().registerKeyframeRule("cube-spin", 1000, "transform", "rotateX(342deg) rotateY(384deg) rotateZ(0deg)");
	StyleSheet::instance().registerRule("cube", "animation", "cube-spin 10s cubic-bezier(0.62, 0, 0.28, 1) infinite");

	const int cubeId = Tree::instance().createView();
	NodeHandle(cubeId).classList().set("cube");

	const auto &style = Tree::instance().node(cubeId).style;
	if (!expectEqual(static_cast<int>(gea::css::AnimationEngine::instance().count()), 0, "priming active animation count")) return 1;
	if (!expectEqual(rstyle(style).transform_rotate_x, -180, "primed rotateX")) return 1;
	if (!expectEqual(rstyle(style).transform_rotate_y, 240, "primed rotateY")) return 1;
	if (!expectEqual(rstyle(style).transform_rotate, 0, "primed rotateZ")) return 1;

	StyleSheet::instance().startCssAnimations(0);
	if (!expectEqual(static_cast<int>(gea::css::AnimationEngine::instance().count()), 3, "started transform animation count")) return 1;
	gea::css::AnimationEngine::instance().tick(5000);
	if (!expectTrue(rstyle(style).transform_rotate_x != -180, "transform animation tick updates rotateX")) return 1;

	resetNativeHost();
	StyleSheet::instance().clear();
	auto &staticSheet = StyleSheet::instance();
	staticSheet.registerStaticPropertyKeyframeRule("fade", 0, Property::Opacity, 0);
	staticSheet.registerStaticPropertyKeyframeRule("fade", 1000, Property::Opacity, 255);
	staticSheet.registerRule("fade-node", "animation", "fade 1s linear forwards");
	staticSheet.registerStaticColorKeyframeRule("ink", 0, StaticStyleColorProperty::Color, 17, 34, 51, 255);
	staticSheet.registerStaticColorKeyframeRule("ink", 1000, StaticStyleColorProperty::Color, 255, 255, 255, 255);
	staticSheet.registerRule("ink-node", "animation", "ink 1s linear forwards");
	staticSheet.registerStaticLengthKeyframeRule("slide", 0, StaticStyleLengthProperty::Left, StaticStyleLengthUnit::Px, 11);
	staticSheet.registerStaticLengthKeyframeRule("slide", 1000, StaticStyleLengthProperty::Left, StaticStyleLengthUnit::Px, 21);
	staticSheet.registerRule("slide-node", "animation", "slide 1s linear forwards");
	staticSheet.registerStaticFilterBlurKeyframeRule("soften", 0, {StaticStyleLengthUnit::Px, 7});
	staticSheet.registerStaticFilterBlurKeyframeRule("soften", 1000, {StaticStyleLengthUnit::Px, 12});
	staticSheet.registerRule("blur-node", "animation", "soften 1s linear forwards");
	staticSheet.registerStaticCustomColorRule(StaticStyleSelectorKind::Class, "theme", "--ink", 0, 255, 0, 255);
	staticSheet.registerKeyframeRule("ink-var", 0, "color", "var(--ink)");
	staticSheet.registerStaticColorKeyframeRule("ink-var", 1000, StaticStyleColorProperty::Color, 255, 255, 255, 255);
	staticSheet.registerRule("ink-var-node", "animation", "ink-var 1s linear forwards");
	staticSheet.registerKeyframeRule("turn", 0, "rotate", "45deg");
	staticSheet.registerKeyframeRule("turn", 1000, "rotate", "90deg");
	staticSheet.registerRule("turn-node", "animation", "turn 1s linear forwards");
	staticSheet.registerKeyframeRule("zoom", 0, "scale", "1.25");
	staticSheet.registerKeyframeRule("zoom", 1000, "scale", "1.5");
	staticSheet.registerRule("zoom-node", "animation", "zoom 1s linear forwards");

	const int fadeId = Tree::instance().createView();
	NodeHandle(fadeId).classList().set("fade-node");
	const int inkId = Tree::instance().createView();
	NodeHandle(inkId).classList().set("ink-node");
	const int slideId = Tree::instance().createView();
	NodeHandle(slideId).classList().set("slide-node");
	const int blurId = Tree::instance().createView();
	NodeHandle(blurId).classList().set("blur-node");
	const int inkVarId = Tree::instance().createView();
	NodeHandle(inkVarId).classList().set("theme ink-var-node");
	const int turnId = Tree::instance().createView();
	NodeHandle(turnId).classList().set("turn-node");
	const int zoomId = Tree::instance().createView();
	NodeHandle(zoomId).classList().set("zoom-node");

	if (!expectEqual(static_cast<int>(gea::css::AnimationEngine::instance().count()), 0, "static keyframe priming active animation count")) return 1;
	if (!expectEqual(Tree::instance().node(fadeId).style.opacity, 0, "static opacity keyframe primed")) return 1;
	if (!expectEqual(static_cast<int>(Tree::instance().node(inkId).style.text_color),
	                 static_cast<int>(gea::framework::graphics::pixel::nativeColor(17, 34, 51)),
	                 "static color keyframe primed")) return 1;
	if (!expectEqual(Tree::instance().node(slideId).style.pos_offsets[3], 11, "static length keyframe primed")) return 1;
	if (!expectEqual(rstyle(Tree::instance().node(blurId).style).filter_blur_radius, 7, "static filter keyframe primed")) return 1;
	if (!expectEqual(static_cast<int>(Tree::instance().node(inkVarId).style.text_color),
	                 static_cast<int>(gea::framework::graphics::pixel::nativeColor(0, 255, 0)),
	                 "compiled color var keyframe primed")) return 1;
	if (!expectEqual(rstyle(Tree::instance().node(turnId).style).rotate_angle, 450, "compiled rotate keyframe primed")) return 1;
	if (!expectEqual(rstyle(Tree::instance().node(zoomId).style).scale_x, 1250, "compiled scale x keyframe primed")) return 1;
	if (!expectEqual(rstyle(Tree::instance().node(zoomId).style).scale_y, 1250, "compiled scale y keyframe primed")) return 1;

	staticSheet.startCssAnimations(0);
	if (!expectEqual(static_cast<int>(gea::css::AnimationEngine::instance().count()), 11, "started static keyframe animation count")) return 1;

	resetNativeHost(); StyleSheet::instance().clear();
	auto &translationSheet = StyleSheet::instance();
	translationSheet.registerKeyframeRule("move", 0, "translate", "none");
	translationSheet.registerKeyframeRule("move", 1000, "translate", "40px 50%");
	translationSheet.registerRule("mover", "transform", "translateX(7px)");
	translationSheet.registerRule("mover", "animation", "move 1s linear forwards");
	const int mover = Tree::instance().createView(); NodeHandle(mover).classList().set("mover");
	const auto &translated = Tree::instance().node(mover).style;
	if (!expectEqual(rstyle(translated).translate_present, 1, "none interpolates as an identity translation context")) return 1;
	translationSheet.startCssAnimations(0); gea::css::AnimationEngine::instance().tick(500);
	if (!expectEqual(rstyle(translated).translate_x, 20, "individual translation keyframe interpolates pixels")) return 1;
	if (!expectEqual(rstyle(translated).translate_y_percent, 250, "individual translation keyframe preserves own-box percentages")) return 1;
	if (!expectEqual(rstyle(translated).transform_translate_x, 7, "individual animation does not overwrite transform list")) return 1;

	// A half-way turn from x:90 to y:90 is 70.529 degrees around (1,1,0),
	// not a 90-degree turn around a linearly blended axis.
	resetNativeHost(); StyleSheet::instance().clear();
	auto &rotations = StyleSheet::instance();
	rotations.registerKeyframeRule("axes", 0, "rotate", "x 90deg");
	rotations.registerKeyframeRule("axes", 1000, "rotate", "90deg y");
	rotations.registerRule("axes-node", "transform", "rotate(30deg)");
	rotations.registerRule("axes-node", "animation", "axes 1s linear forwards");
	rotations.registerKeyframeRule("identity", 0, "rotate", "none");
	rotations.registerKeyframeRule("identity", 1000, "rotate", "x 180deg");
	rotations.registerRule("identity-node", "animation", "identity 1s linear forwards");
	rotations.registerKeyframeRule("turns", 0, "rotate", "x 45deg");
	rotations.registerKeyframeRule("turns", 1000, "rotate", "x 765deg");
	rotations.registerRule("turns-node", "animation", "turns 1s linear forwards");
	rotations.registerKeyframeRule("stretch", 0, "scale", "none");
	rotations.registerKeyframeRule("stretch", 1000, "scale", "-1 3 2");
	rotations.registerRule("stretch-node", "animation", "stretch 1s linear forwards");
	const int axesId = Tree::instance().createView(); NodeHandle(axesId).classList().set("axes-node");
	const int identityId = Tree::instance().createView(); NodeHandle(identityId).classList().set("identity-node");
	const int turnsId = Tree::instance().createView(); NodeHandle(turnsId).classList().set("turns-node");
	const int stretchId = Tree::instance().createView(); NodeHandle(stretchId).classList().set("stretch-node");
	rotations.startCssAnimations(0); gea::css::AnimationEngine::instance().tick(500);
	const auto &axes = rstyle(Tree::instance().node(axesId).style);
	if (!expectEqual(axes.rotate_angle, 705, "different axes use spherical rotation interpolation")) return 1;
	if (!expectEqual(axes.rotate_axis_x, 707107, "spherical midpoint x axis")) return 1;
	if (!expectEqual(axes.rotate_axis_y, 707107, "spherical midpoint y axis")) return 1;
	if (!expectEqual(axes.rotate_axis_z, 0, "spherical midpoint z axis")) return 1;
	if (!expectEqual(axes.transform_rotate, 300, "rotation animation preserves list rotation")) return 1;
	const auto &identity = rstyle(Tree::instance().node(identityId).style);
	if (!expectEqual(identity.rotate_angle, 900, "none endpoint interpolates to half turn")) return 1;
	if (!expectEqual(identity.rotate_axis_x, 1000000, "identity endpoint adopts destination axis")) return 1;
	if (!expectEqual(identity.rotate_axis_z, 0, "identity endpoint does not introduce z rotation")) return 1;
	if (!expectEqual(rstyle(Tree::instance().node(turnsId).style).rotate_angle, 4050, "same-axis interpolation preserves authored full turns")) return 1;
	const auto &stretch = rstyle(Tree::instance().node(stretchId).style);
	if (!expectEqual(stretch.scale_x, 0, "signed scaling interpolates through zero")) return 1;
	if (!expectEqual(stretch.scale_y, 2000, "nonuniform y scale interpolation")) return 1;
	if (!expectEqual(stretch.scale_z, 1500, "independent depth scale interpolation")) return 1;

	return 0;
}
