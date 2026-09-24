// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace gea::embedded::ui {

int resolveLayoutSizeExpression(int nodeId, int expression, bool horizontal);
int resolveLineHeightExpression(int nodeId, int expression);
int resolveLineHeightMultiplier(int nodeId, int bits);
int resolveLayoutFlexBasis(int nodeId, int percentageBasis, bool horizontal);
bool resolveLayoutBoxLengths(int nodeId, int percentageBasis);
bool layoutSizeExpressionUsesPercentage(int nodeId, int expression);

enum class Property : int {
	Display = 0,
	FlexDirection,
	FlexWrap,
	JustifyContent,
	AlignItems,
	JustifyItems,
	AlignContent,
	AlignSelf,
	Gap,
	Width,
	Height,
	WidthPercent,
	HeightPercent,
	MinWidth,
	MinHeight,
	MaxWidth,
	MaxHeight,
	Flex,
	FlexShrink,
	FlexBasis,
	PaddingTop,
	PaddingRight,
	PaddingBottom,
	PaddingLeft,
	MarginTop,
	MarginRight,
	MarginBottom,
	MarginLeft,
	Position,
	Top,
	Right,
	Bottom,
	Left,
	TopPercent,
	RightPercent,
	BottomPercent,
	LeftPercent,
	ZIndex,
	BackgroundColor,
	HasBackground,
	ActiveBackgroundColor,
	HasActiveBackground,
	Color,
	Opacity,
	BlinkInterval,
	BorderWidth,
	BorderColor,
	BorderTopWidth,
	BorderRightWidth,
	BorderBottomWidth,
	BorderLeftWidth,
	BorderTopColor,
	BorderRightColor,
	BorderBottomColor,
	BorderLeftColor,
	BorderRadiusTopLeft,
	BorderRadiusTopRight,
	BorderRadiusBottomRight,
	BorderRadiusBottomLeft,
	BorderRadiusTopLeftPercent,
	BorderRadiusTopRightPercent,
	BorderRadiusBottomRightPercent,
	BorderRadiusBottomLeftPercent,
	FontId,
	FontSize,
	FontWeight,
	LineHeight,
	TextAlign,
	Overflow,
	OverflowX,
	OverflowY,
	MaskRightFadeWidth,
	ImageId,
	ImageFit,
	TransformRotate,
	TransformRotateX,
	TransformRotateY,
	TransformTranslateX,
	TransformTranslateY,
	TransformTranslateZ,
	TransformTranslateXPercent,
	TransformTranslateYPercent,
	TransformScaleX,
	TransformScaleY,
	TransformScaleZ,
	TransformOriginX,
	TransformOriginY,
	Perspective,
	PerspectiveOriginX,
	PerspectiveOriginY,
	FilterBlur,
	BoxShadowInset,
	BoxShadowOffsetX,
	BoxShadowOffsetY,
	BoxShadowBlur,
	BoxShadowSpread,
	BoxShadowColor,
	BoxShadowAlpha,
	TextDecoration,
	TextTransform,
	WhiteSpace,
	TextOverflow,
	Backface,
	PointerEvents,
	Order,
	BoxSizing,
	Float,
	Clear,
	WritingMode,
	RowGap,
	ColumnGap,
	RowGapPercent,
	ColumnGapPercent,
	MarginTopAuto,
	MarginRightAuto,
	MarginBottomAuto,
	MarginLeftAuto,
	WidthExpression,
	HeightExpression,
	Direction,
	MarginTopExpression,
	MarginRightExpression,
	MarginBottomExpression,
	MarginLeftExpression,
	PaddingTopExpression,
	PaddingRightExpression,
	PaddingBottomExpression,
	PaddingLeftExpression,
	JustifySelf,
	GridRowStart,
	GridColumnStart,
	GridRowEnd,
	GridColumnEnd,
	TransformPresent,
	RotatePresent,
	ScalePresent,
	FilterPresent,
	AspectRatio,
	FlexLineCount,
	BorderColorCurrent,
	BorderTopColorCurrent,
	BorderRightColorCurrent,
	BorderBottomColorCurrent,
	BorderLeftColorCurrent,
	ColorAlpha,
	BorderAlpha,
	BorderTopAlpha,
	BorderRightAlpha,
	BorderBottomAlpha,
	BorderLeftAlpha,
	BackgroundAlpha,
	BackgroundImage,
	Containment,
	BackgroundClip,
	FlexBasisExpression,
	LineHeightExpression,
	TransformStyle,
	LineHeightMultiplier,
	Visibility,
	TranslatePresent,
	TranslateX,
	TranslateY,
	TranslateZ,
	TranslateXPercent,
	TranslateYPercent,
	TransformTranslateOuterAxes,
	RotateAngle,
	RotateAxisX,
	RotateAxisY,
	RotateAxisZ,
	ScaleX,
	ScaleY,
	ScaleZ,
	BackgroundSizeList,
	BackgroundPositionList,
	BackgroundRepeatList,
	BackgroundAttachmentList,
	BackgroundOriginList,
	MarginTrim,
	BorderTopRelief,
	BorderRightRelief,
	BorderBottomRelief,
	BorderLeftRelief,
	BorderRelief,
	Count
};

enum class StyleDeclaration : std::uint8_t {
	Unknown,
	Ignored,
	Custom,
	Animation,
	Display,
	FlexDirection,
	FlexWrap,
	JustifyContent,
	AlignItems,
	JustifyItems,
	AlignContent,
	AlignSelf,
	PlaceItems,
	GridTemplateColumns,
	GridTemplateRows,
	Content,
	Gap,
	Width,
	Height,
	MinWidth,
	MinHeight,
	MaxWidth,
	MaxHeight,
	Flex,
	FlexGrow,
	FlexShrink,
	FlexBasis,
	Padding,
	PaddingTop,
	PaddingRight,
	PaddingBottom,
	PaddingLeft,
	Margin,
	MarginTop,
	MarginRight,
	MarginBottom,
	MarginLeft,
	Position,
	Inset,
	Top,
	Right,
	Bottom,
	Left,
	ZIndex,
	ActiveBackgroundColor,
	Background,
	BackgroundSize,
	ObjectFit,
	Color,
	Opacity,
	BorderColor,
	Border,
	BorderWidth,
	BorderTop,
	BorderRight,
	BorderBottom,
	BorderLeft,
	BorderTopWidth,
	BorderRightWidth,
	BorderBottomWidth,
	BorderLeftWidth,
	BorderTopColor,
	BorderRightColor,
	BorderBottomColor,
	BorderLeftColor,
	BorderRadius,
	BorderTopLeftRadius,
	BorderTopRightRadius,
	BorderBottomRightRadius,
	BorderBottomLeftRadius,
	FontFamily,
	FontSize,
	FontWeight,
	LineHeight,
	TextAlign,
	TextDecoration,
	TextTransform,
	WhiteSpace,
	TextOverflow,
	BackfaceVisibility,
	PointerEvents,
	Overflow,
	OverflowX,
	OverflowY,
	MaskImage,
	Transform,
	Rotate,
	Scale,
	Filter,
	BoxShadow,
	TransformOrigin,
	Perspective,
	PerspectiveOrigin,
	Order,
	BoxSizing,
	Float,
	Clear,
	WritingMode,
	FlexFlow,
	RowGap,
	ColumnGap,
	Direction,
	Font,
	JustifySelf,
	GridTemplate,
	Grid,
	GridRowStart,
	GridColumnStart,
	GridRowEnd,
	GridColumnEnd,
	GridRow,
	GridColumn,
	GridArea,
	AspectRatio,
	FlexLineCount,
	PlaceContent,
	PlaceSelf,
	BackgroundColor,
	BackgroundImage,
	Contain,
	BackgroundClip,
	TransformStyle,
	Visibility,
	Translate,
	BackgroundPosition,
	BackgroundRepeat,
	BackgroundAttachment,
	BackgroundOrigin,
	MarginTrim
};

class Style {
public:
	explicit Style(int nodeId = -1) : nodeId_(nodeId) {}

	int nodeId() const { return nodeId_; }
	void set(Property property, int value) const;

	void width(int value) const { set(Property::Width, value); }
	void height(int value) const { set(Property::Height, value); }
	void backgroundColor(int rgb565) const;
	void color(int rgb565) const { set(Property::Color, rgb565); }
	void opacity(int value) const { set(Property::Opacity, value); }
	void display(int value) const { set(Property::Display, value); }
	void position(int value) const { set(Property::Position, value); }
	void left(int value) const { set(Property::Left, value); }
	void top(int value) const { set(Property::Top, value); }
	void translateX(int value) const { set(Property::TransformTranslateX, value); }
	void translateY(int value) const { set(Property::TransformTranslateY, value); }
	void rotateDegrees(double value) const;
	void scale(double value) const;
	void cssRotateDegrees(double value) const;
	void cssScale(double value) const;
	void imageId(int value) const { set(Property::ImageId, value); }
	void imageFit(int value) const { set(Property::ImageFit, value); }

	// Generic string-keyed inline style access — the `CSSStyleDeclaration`
	// shape (`style.setProperty(name, value)` / `style.removeProperty(name)`)
	// gea's own `reactiveStyle`/`reactiveStyleProp` runtime helpers use
	// (packages/gea/src/runtime/reactive-style.ts). Delegates to
	// `StyleSheet`'s existing NodeHandle+string inline-style path (the same
	// one the CSS parser feeds), rather than duplicating property-name
	// parsing here.
	void setProperty(const std::string &property, const std::string &value) const;
	bool removeProperty(const std::string &property) const;

private:
	int nodeId_;
};

class NodeHandle;

enum class StaticStyleSelectorKind : std::uint8_t {
	Class,
	Element,
	Selector
};

struct StaticStyleSimpleSelectorSpec {
	const char *tag = nullptr;
	const char *id = nullptr;
	std::initializer_list<const char *> classes = {};
	bool wantsRoot = false;
	bool wantsFirstChild = false;
	bool wantsLastChild = false;
	bool wantsHover = false;
};

struct StaticStyleSelectorPartSpec {
	StaticStyleSimpleSelectorSpec simple;
	bool directParent = false;
};

enum class StaticStyleMediaFeatureKind : std::uint8_t {
	AlwaysFalse,
	Orientation,
	Monochrome,
	Width,
	Height,
	AspectRatio,
	Resolution
};

enum class StaticStyleMediaCompare : std::uint8_t {
	Equal,
	Min,
	Max,
	Boolean
};

struct StaticStyleMediaTermSpec {
	StaticStyleMediaFeatureKind kind = StaticStyleMediaFeatureKind::AlwaysFalse;
	StaticStyleMediaCompare compare = StaticStyleMediaCompare::Equal;
	double value = 0.0;
	std::uint8_t orientation = 0;
};

struct StaticStyleMediaQuerySpec {
	std::initializer_list<StaticStyleMediaTermSpec> terms = {};
	bool valid = true;
};

enum class StaticStyleColorProperty : std::uint8_t {
	Color,
	Background,
	ActiveBackground,
	Border,
	BorderTop,
	BorderRight,
	BorderBottom,
	BorderLeft,
	BackgroundColor
};

enum class StaticStyleLengthUnit : std::uint8_t {
	Raw,
	Px,
	Percent,
	Vw,
	Vh,
	Vmin,
	Vmax,
	Dvw,
	Dvh,
	Auto,
	Expression
};

enum class StaticStyleLengthProperty : std::uint8_t {
	Gap,
	Width,
	Height,
	MinWidth,
	MinHeight,
	MaxWidth,
	MaxHeight,
	FlexBasis,
	PaddingTop,
	PaddingRight,
	PaddingBottom,
	PaddingLeft,
	MarginTop,
	MarginRight,
	MarginBottom,
	MarginLeft,
	BorderWidth,
	BorderTopWidth,
	BorderRightWidth,
	BorderBottomWidth,
	BorderLeftWidth,
	FontSize,
	Perspective,
	MaskImage,
	Top,
	Right,
	Bottom,
	Left
};

enum class StaticStyleBorderRadiusCorner : std::uint8_t {
	TopLeft,
	TopRight,
	BottomRight,
	BottomLeft
};

enum class StaticStyleOriginProperty : std::uint8_t {
	TransformOrigin,
	PerspectiveOrigin
};

enum class StaticStyleGridTemplateProperty : std::uint8_t {
	Columns,
	Rows
};

enum class StaticStyleLengthExpressionKind : std::uint8_t {
	Add,
	Subtract,
	Multiply,
	Divide,
	Min,
	Max,
	Clamp,
	Var
};

enum class StaticStyleLineHeightKind : std::uint8_t {
	Normal,
	Scalar,
	Percent,
	Length
};

struct StaticStylePropertyValue {
	Property property;
	int value;
};

struct StaticStyleLengthSpec {
	StaticStyleLengthUnit unit;
	float value;
};

struct StaticStyleColor {
	int r;
	int g;
	int b;
	int a;
};

struct StaticStyleColorRef {
	const char *varName;
	StaticStyleColor fallback;
	bool hasFallback;
};

struct StaticStyleLinearGradient {
	int angleTenths;
	StaticStyleColor from;
	StaticStyleColor mid;
	StaticStyleColor to;
	int midStopPermille;
	int toStopPermille;
	bool hasMid;
};

struct StaticStyleLinearGradientRef {
	int angleTenths;
	StaticStyleColorRef from;
	StaticStyleColorRef mid;
	StaticStyleColorRef to;
	int midStopPermille;
	int toStopPermille;
	bool hasMid;
};

struct StaticStyleRadialGradientRef {
	StaticStyleColorRef from;
	StaticStyleColorRef to;
	int stopPermille;
	int cxPermille;
	int cyPermille;
	int rxPermille;
	int ryPermille;
	bool enabled;
};

struct StaticStyleGridTemplateTrack {
	int type;
	int value;
	StaticStyleLengthSpec length;
};

struct StaticStyleBackgroundGridLine {
	bool enabled;
	StaticStyleLengthSpec width;
	StaticStyleColor color;
};

enum class StaticStyleAnimationDirection : std::uint8_t {
	Normal,
	Reverse,
	Alternate,
	AlternateReverse
};

enum class StaticStyleAnimationFill : std::uint8_t {
	None,
	Forwards,
	Backwards,
	Both
};

enum class StaticStyleAnimationEasingKind : std::uint8_t {
	Linear,
	Ease,
	EaseIn,
	EaseOut,
	EaseInOut,
	CubicBezier,
	Steps
};

struct StaticStyleAnimationEasing {
	StaticStyleAnimationEasingKind kind;
	double x1;
	double y1;
	double x2;
	double y2;
	int steps;
};

class StyleSheet {
public:
	static StyleSheet &instance();

	void clear();
	void beginRuleRegistrationBatch();
	void endRuleRegistrationBatch();
	void registerRule(const std::string &className, const std::string &property, const std::string &value, const std::string &media = std::string());
	void registerElementRule(const std::string &elementName, const std::string &property, const std::string &value, const std::string &media = std::string());
	// Defaults have lower cascade priority than every author selector.
	void registerUserAgentElementRule(const std::string &elementName, const std::string &property, const std::string &value);
	void registerSelectorRule(const std::string &selector, const std::string &property, const std::string &value, const std::string &media = std::string());
	void registerKeyframeRule(const std::string &name, int offsetPermille, const std::string &property, const std::string &value);
	void registerStaticRule(const char *className, const char *property, const char *value, const char *media = nullptr);
	void registerStaticElementRule(const char *elementName, const char *property, const char *value, const char *media = nullptr);
	void registerStaticSelectorRule(const char *selector, const char *property, const char *value, const char *media = nullptr);
	void registerStaticKeyframeRule(const char *name, int offsetPermille, const char *property, const char *value);
	void registerStaticPropertyKeyframeRule(const char *name, int offsetPermille, Property property, int value);
	void registerStaticColorKeyframeRule(const char *name,
	                                     int offsetPermille,
	                                     StaticStyleColorProperty property,
	                                     int r,
	                                     int g,
	                                     int b,
	                                     int a = 255);
	void registerStaticColorVarKeyframeRule(const char *name,
	                                        int offsetPermille,
	                                        StaticStyleColorProperty property,
	                                        const char *varName,
	                                        bool hasFallback,
	                                        int r,
	                                        int g,
	                                        int b,
	                                        int a = 255);
	void registerStaticLengthKeyframeRule(const char *name,
	                                      int offsetPermille,
	                                      StaticStyleLengthProperty property,
	                                      StaticStyleLengthUnit unit,
	                                      float value);
	void registerStaticLengthSpecKeyframeRule(const char *name,
	                                          int offsetPermille,
	                                          StaticStyleLengthProperty property,
	                                          StaticStyleLengthSpec length);
	void registerStaticFilterBlurKeyframeRule(const char *name,
	                                          int offsetPermille,
	                                          StaticStyleLengthSpec radius);
	void registerStaticTransformKeyframeRule(const char *name,
	                                         int offsetPermille,
	                                         std::uint16_t flags,
	                                         int rotateX,
	                                         int rotateY,
	                                         int rotateZ,
	                                         StaticStyleLengthSpec translateX,
	                                         StaticStyleLengthSpec translateY,
	                                         StaticStyleLengthSpec translateZ,
	                                         int scaleX,
	                                         int scaleY,
	                                         int scaleZ = 1000);
	void registerStaticPropertyRule(StaticStyleSelectorKind selectorKind,
	                                const char *selector,
	                                Property property,
	                                int value,
	                                const char *media = nullptr);
	void registerStaticPropertyGroupRule(StaticStyleSelectorKind selectorKind,
	                                     const char *selector,
	                                     std::initializer_list<StaticStylePropertyValue> properties,
	                                     const char *media = nullptr);
	std::uint16_t registerStaticSelectorPlan(const char *selector,
	                                         std::initializer_list<StaticStyleSelectorPartSpec> parts);
	std::uint16_t registerStaticMediaConditionPlan(const char *condition,
	                                               std::initializer_list<StaticStyleMediaQuerySpec> queries);
	void registerStaticColorRule(StaticStyleSelectorKind selectorKind,
	                             const char *selector,
	                             StaticStyleColorProperty property,
	                             int r,
	                             int g,
	                             int b,
	                             int a = 255,
	                             const char *media = nullptr);
	void registerStaticColorVarRule(StaticStyleSelectorKind selectorKind,
	                                const char *selector,
	                                StaticStyleColorProperty property,
	                                const char *name,
	                                bool hasFallback,
	                                int r,
	                                int g,
	                                int b,
	                                int a = 255,
	                                const char *media = nullptr);
	void registerStaticLengthRule(StaticStyleSelectorKind selectorKind,
	                              const char *selector,
	                              StaticStyleLengthProperty property,
	                              StaticStyleLengthUnit unit,
	                              float value,
	                              const char *media = nullptr);
	std::uint16_t registerStaticLengthExpression(StaticStyleLengthExpressionKind kind,
	                                             StaticStyleLengthSpec a,
	                                             StaticStyleLengthSpec b,
	                                             StaticStyleLengthSpec c,
	                                             float scalar,
	                                             const char *name,
	                                             bool hasFallback);
	void registerStaticLengthSpecRule(StaticStyleSelectorKind selectorKind,
	                                  const char *selector,
	                                  StaticStyleLengthProperty property,
	                                  StaticStyleLengthSpec length,
	                                  const char *media = nullptr);
	void registerStaticFontFamilyRule(StaticStyleSelectorKind selectorKind,
	                                  const char *selector,
	                                  const char *family,
	                                  const char *media = nullptr);
	void registerStaticLineHeightRule(StaticStyleSelectorKind selectorKind,
	                                  const char *selector,
	                                  StaticStyleLineHeightKind kind,
	                                  StaticStyleLengthSpec value,
	                                  const char *media = nullptr);
	void registerStaticFlexRule(StaticStyleSelectorKind selectorKind,
	                            const char *selector,
	                            int grow,
	                            StaticStyleLengthSpec basis,
	                            bool hasBasis,
	                            const char *media = nullptr);
	void registerStaticBorderRule(StaticStyleSelectorKind selectorKind,
	                              const char *selector,
	                              StaticStyleLengthSpec width,
	                              int r,
	                              int g,
	                              int b,
	                              int a = 255,
	                              const char *media = nullptr);
	void registerStaticBorderRadiusRule(StaticStyleSelectorKind selectorKind,
	                                    const char *selector,
	                                    StaticStyleLengthSpec topLeft,
	                                    StaticStyleLengthSpec topRight,
	                                    StaticStyleLengthSpec bottomRight,
	                                    StaticStyleLengthSpec bottomLeft,
	                                    const char *media = nullptr);
	void registerStaticBorderRadiusCornerRule(StaticStyleSelectorKind selectorKind,
	                                          const char *selector,
	                                          StaticStyleBorderRadiusCorner corner,
	                                          StaticStyleLengthSpec radius,
	                                          const char *media = nullptr);
	void registerStaticFilterBlurRule(StaticStyleSelectorKind selectorKind,
	                                  const char *selector,
	                                  StaticStyleLengthSpec radius,
	                                  const char *media = nullptr);
	void registerStaticBoxShadowNoneRule(StaticStyleSelectorKind selectorKind,
	                                     const char *selector,
	                                     const char *media = nullptr);
	void registerStaticBackgroundRule(StaticStyleSelectorKind selectorKind,
	                                  const char *selector,
	                                  StaticStyleLinearGradient gradient,
	                                  StaticStyleLinearGradient overlayGradient,
	                                  bool hasOverlayGradient,
	                                  StaticStyleBackgroundGridLine gridX,
	                                  StaticStyleBackgroundGridLine gridY,
	                                  const char *media = nullptr,
	                                      bool imageOnly = false);
	void registerStaticBackgroundFullRule(StaticStyleSelectorKind selectorKind,
	                                      const char *selector,
	                                      StaticStyleLinearGradientRef gradient,
	                                      StaticStyleLinearGradientRef overlayGradient,
	                                      bool hasOverlayGradient,
	                                      StaticStyleRadialGradientRef radialGradient,
	                                      StaticStyleBackgroundGridLine gridX,
	                                      StaticStyleBackgroundGridLine gridY,
	                                      const char *media = nullptr,
	                                      bool imageOnly = false);
	void registerStaticBackgroundSizeRule(StaticStyleSelectorKind selectorKind,
	                                      const char *selector,
	                                      StaticStyleLengthSpec stepX,
	                                      StaticStyleLengthSpec stepY,
	                                      const char *media = nullptr);
	void registerStaticGridTemplateRule(StaticStyleSelectorKind selectorKind,
	                                    const char *selector,
	                                    StaticStyleGridTemplateProperty property,
	                                    std::initializer_list<StaticStyleGridTemplateTrack> tracks,
	                                    const char *media = nullptr);
	void registerStaticAnimationRule(StaticStyleSelectorKind selectorKind,
	                                 const char *selector,
	                                 const char *name,
	                                 std::uint32_t durationMs,
	                                 std::uint32_t delayMs,
	                                 int iterations,
	                                 StaticStyleAnimationDirection direction,
	                                 StaticStyleAnimationFill fill,
	                                 StaticStyleAnimationEasing easing,
	                                 const char *media = nullptr);
	void registerStaticCustomLengthRule(StaticStyleSelectorKind selectorKind,
	                                    const char *selector,
	                                    const char *name,
	                                    StaticStyleLengthSpec length,
	                                    const char *media = nullptr);
	void registerStaticCustomColorRule(StaticStyleSelectorKind selectorKind,
	                                   const char *selector,
	                                   const char *name,
	                                   int r,
	                                   int g,
	                                   int b,
	                                   int a = 255,
	                                   const char *media = nullptr);
	void registerStaticTransformRule(StaticStyleSelectorKind selectorKind,
	                                 const char *selector,
	                                 std::uint16_t flags,
	                                 int rotateX,
	                                 int rotateY,
	                                 int rotateZ,
	                                 StaticStyleLengthSpec translateX,
	                                 StaticStyleLengthSpec translateY,
	                                 StaticStyleLengthSpec translateZ,
	                                 int scaleX,
	                                 int scaleY,
	                                 const char *media = nullptr,
	                                 int scaleZ = 1000);
	void registerStaticOriginRule(StaticStyleSelectorKind selectorKind,
	                              const char *selector,
	                              StaticStyleOriginProperty property,
	                              int xPermille,
	                              int yPermille,
	                              const char *media = nullptr);
	void applyClass(NodeHandle node, const std::string &className) const;
		void setClassName(NodeHandle node, const std::string &className) const;
		void applyProperty(NodeHandle node, StyleDeclaration declaration, const std::string &value) const;
		void applyProperty(NodeHandle node, const char *property, const std::string &value) const;
		void applyProperty(NodeHandle node, const std::string &property, const std::string &value) const;
		// Preserve CSS px units through device scaling before property-specific rounding.
		bool applyPixelLengthProperty(NodeHandle node, StyleDeclaration declaration, double value) const;
		bool applyNumberProperty(NodeHandle node, StyleDeclaration declaration, double value) const;
		bool applyNumberProperty(NodeHandle node, const char *property, double value) const;
		bool removeProperty(NodeHandle node, const std::string &property) const;
	void recomputeSubtree(int nodeId) const;
	void hoverChanged() const;
	void startCssAnimations(std::uint32_t nowMs) const;

private:
	StyleSheet() = default;
};

void setViewportMetrics(int width, int height, double devicePixelRatio);
double devicePixelRatio();
void setDevicePixelRatio(double devicePixelRatio);
void applyAnimatedStyleValue(int nodeId, Property property, int value);
void setSafeAreaInsetBottom(int inset);
int safeAreaInsetBottom();

}  // namespace gea::embedded::ui
