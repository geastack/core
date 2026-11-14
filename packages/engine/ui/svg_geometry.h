// SPDX-License-Identifier: Apache-2.0
#pragma once

// Inline SVG geometry: everything that turns SVG source text into pixels,
// with no dependency on the node tree. The engine's <svg> element
// (svg_element.h) resolves attributes and CSS into calls on this header; the
// unit tests drive it directly.
//
//   parse   -- path data (M L H V C S Q T A Z, absolute and relative), point
//              lists, transform lists, colours, lengths.
//   flatten -- curves and elliptical arcs become line segments, in device
//              space, to a fixed pixel tolerance.
//   stroke  -- a polyline becomes filled polygons (segment quads, joins, caps)
//              that all wind the same way, so a nonzero fill is their union.
//   raster  -- exact-area anti-aliased coverage (signed-area accumulation),
//              strip by strip, emitted one coverage row at a time.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <new>
#include <vector>

#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#define GEA_SVG_HOT IRAM_ATTR
#else
#define GEA_SVG_HOT
#endif

namespace gea::embedded::ui::svg {

// Scratch the rasterizer touches per pixel lives in internal RAM on ESP32: a
// PSRAM accumulator costs a cache miss per touch and doubles paint time.
template <typename T>
struct InternalAllocator {
	using value_type = T;
	InternalAllocator() = default;
	template <typename U>
	InternalAllocator(const InternalAllocator<U> &) {}
	T *allocate(std::size_t n)
	{
#if defined(ESP_PLATFORM)
		if (void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return static_cast<T *>(p);
		if (void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT)) return static_cast<T *>(p);
		throw std::bad_alloc();
#else
		return std::allocator<T>{}.allocate(n);
#endif
	}
	void deallocate(T *p, std::size_t n)
	{
#if defined(ESP_PLATFORM)
		(void)n;
		heap_caps_free(p);
#else
		std::allocator<T>{}.deallocate(p, n);
#endif
	}
	template <typename U>
	bool operator==(const InternalAllocator<U> &) const { return true; }
	template <typename U>
	bool operator!=(const InternalAllocator<U> &) const { return false; }
};
template <typename T>
using ScratchVector = std::vector<T, InternalAllocator<T>>;

// Data read in order, a few times per paint (a shape's edge list), goes to
// PSRAM on ESP32: the cache serves sequential reads well, and internal RAM is
// left to the accumulators, which are touched at random once per cell.
template <typename T>
struct ExternalAllocator {
	using value_type = T;
	ExternalAllocator() = default;
	template <typename U>
	ExternalAllocator(const ExternalAllocator<U> &) {}
	T *allocate(std::size_t n)
	{
#if defined(ESP_PLATFORM)
		if (void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return static_cast<T *>(p);
		if (void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT)) return static_cast<T *>(p);
		throw std::bad_alloc();
#else
		return std::allocator<T>{}.allocate(n);
#endif
	}
	void deallocate(T *p, std::size_t n)
	{
#if defined(ESP_PLATFORM)
		(void)n;
		heap_caps_free(p);
#else
		std::allocator<T>{}.deallocate(p, n);
#endif
	}
	template <typename U>
	bool operator==(const ExternalAllocator<U> &) const { return true; }
	template <typename U>
	bool operator!=(const ExternalAllocator<U> &) const { return false; }
};

// Grows a zero-filled scratch vector to `need` elements, keeping it in
// internal RAM. The allocator falls back to PSRAM when internal RAM is short
// at that moment (during boot, say), and a vector never moves on its own, so
// one that landed outside retries now and then and moves back in once a block
// is free. Capacity is reserved up front so the usual sizes never reallocate.
template <typename V>
inline void growInternalZeroed(V &v, std::size_t need, std::size_t reserve, int &retry)
{
#if defined(ESP_PLATFORM)
	const bool inside = v.capacity() == 0 || esp_ptr_internal(v.data());
	if (v.size() >= need && (inside || (++retry & 63) != 0)) return;
	V fresh;
	fresh.reserve(std::max(need, reserve));
	if (v.size() >= need && !esp_ptr_internal(fresh.data())) return;  // no better block yet
	fresh.resize(std::max(need, v.size()), 0);
	v.swap(fresh);
#else
	(void)reserve;
	(void)retry;
	if (v.size() < need) v.resize(need, 0);
#endif
}

struct Point {
	float x = 0.0f;
	float y = 0.0f;
};

// Affine transform [a c e; b d f; 0 0 1], the SVG matrix(a b c d e f) order.
struct Matrix {
	float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

	Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
	// this * other: `other` applies first.
	Matrix operator*(const Matrix &o) const
	{
		return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e,
		        b * o.e + d * o.f + f};
	}
	// Geometric-mean scale: how much a unit length grows (stroke widths).
	float scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
	static Matrix translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
	static Matrix scaling(float x, float y) { return {x, 0, 0, y, 0, 0}; }
	static Matrix rotate(float degrees)
	{
		const float r = degrees * 3.14159265358979f / 180.0f;
		const float cs = std::cos(r), sn = std::sin(r);
		return {cs, sn, -sn, cs, 0, 0};
	}
};

using Contour = std::vector<Point>;

// A flattened path: contours of device-space points, each open or closed.
struct Path {
	std::vector<Contour> contours;
	std::vector<std::uint8_t> closed;
	bool empty() const { return contours.empty(); }
	void clear() { contours.clear(); closed.clear(); }
};

// ---------------------------------------------------------------------------
// Number scanning (SVG number grammar: 1e-3, .5.5, -1-2, flags without commas)

class Scanner {
public:
	explicit Scanner(const char *s) : p_(s ? s : "") {}

	void skipSeparators()
	{
		while (*p_ && (std::isspace(static_cast<unsigned char>(*p_)) || *p_ == ',')) ++p_;
	}
	void skipSpaces()
	{
		while (*p_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
	}
	bool atEnd()
	{
		skipSeparators();
		return *p_ == '\0';
	}
	char peek()
	{
		skipSeparators();
		return *p_;
	}
	bool startsNumber()
	{
		skipSeparators();
		const char c = *p_;
		return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
	}
	bool number(float &out)
	{
		skipSeparators();
		const char *start = p_;
		const char *q = p_;
		if (*q == '+' || *q == '-') ++q;
		bool digits = false;
		while (*q >= '0' && *q <= '9') { ++q; digits = true; }
		if (*q == '.') {
			++q;
			while (*q >= '0' && *q <= '9') { ++q; digits = true; }
		}
		if (!digits) return false;
		bool exponent = false;
		if (*q == 'e' || *q == 'E') {
			const char *r = q + 1;
			if (*r == '+' || *r == '-') ++r;
			if (*r >= '0' && *r <= '9') {
				while (*r >= '0' && *r <= '9') ++r;
				q = r;
				exponent = true;
			}
		}
		out = exponent ? slowNumber(start, q) : fastNumber(start, q);
		p_ = q;
		return true;
	}
	// Arc flags are a single 0/1 digit and may run into the next number.
	bool flag(bool &out)
	{
		skipSeparators();
		if (*p_ == '0' || *p_ == '1') {
			out = *p_ == '1';
			++p_;
			return true;
		}
		return false;
	}
	char take() { return *p_ ? *p_++ : '\0'; }
	const char *rest() const { return p_; }
	void advance(std::size_t n) { while (n-- && *p_) ++p_; }

private:
	// Path data is re-parsed on every attribute change, so a number must not
	// cost a heap string and a software-double strtod (soft-float on ESP32).
	// Digits accumulate into an integer: a whole number converts straight to
	// float, and a decimal whose digits fit float's 24-bit mantissa is one
	// IEEE float division by an exact power of ten (correctly rounded, like
	// strtod-then-narrow). Anything wider takes the strtod path.
	static float fastNumber(const char *s, const char *end)
	{
		static constexpr float pow10[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
		const char *q = s;
		const bool negative = *q == '-';
		if (*q == '+' || *q == '-') ++q;
		std::uint32_t mantissa = 0;
		int fraction = -1;
		for (; q < end; ++q) {
			if (*q == '.') {
				fraction = 0;
				continue;
			}
			if (mantissa > (1u << 24) / 10) return slowNumber(s, end);
			mantissa = mantissa * 10 + static_cast<std::uint32_t>(*q - '0');
			if (fraction >= 0 && ++fraction > 10) return slowNumber(s, end);
		}
		if (mantissa > (1u << 24)) return slowNumber(s, end);
		float v = static_cast<float>(mantissa);
		if (fraction > 0) v /= pow10[fraction];
		return negative ? -v : v;
	}
	static float slowNumber(const char *s, const char *end)
	{
		char buf[64];
		std::size_t n = static_cast<std::size_t>(end - s);
		if (n >= sizeof buf) n = sizeof buf - 1;
		std::memcpy(buf, s, n);
		buf[n] = '\0';
		return static_cast<float>(std::strtod(buf, nullptr));
	}

	const char *p_;
};

// A length in user units: "12", "12px", "1.5e1". Percentages and other units
// are refused (returns false) so the caller keeps its fallback.
inline bool parseLength(const char *s, float &out)
{
	if (!s) return false;
	Scanner sc(s);
	float v = 0;
	if (!sc.number(v)) return false;
	sc.skipSpaces();
	const char *rest = sc.rest();
	if (*rest == '\0' || std::strncmp(rest, "px", 2) == 0) {
		out = v;
		return true;
	}
	return false;
}

inline bool parseNumberList(const char *s, float *out, int count)
{
	Scanner sc(s);
	for (int i = 0; i < count; ++i)
		if (!sc.number(out[i])) return false;
	return true;
}

// ---------------------------------------------------------------------------
// Colours

struct Color {
	std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

enum class PaintKind : std::uint8_t { Unset, None, CurrentColor, Color, Url };

struct Paint {
	PaintKind kind = PaintKind::Unset;
	Color color;
	char ref[32] = {};  // Url: the referenced element's id
};

namespace detail {
inline int hexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

inline bool iequalsPrefix(const char *s, const char *word)
{
	for (; *word; ++s, ++word)
		if (std::tolower(static_cast<unsigned char>(*s)) != *word) return false;
	return true;
}

inline float hueToRgb(float p, float q, float t)
{
	if (t < 0) t += 1;
	if (t > 1) t -= 1;
	if (t < 1.0f / 6) return p + (q - p) * 6 * t;
	if (t < 0.5f) return q;
	if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
	return p;
}

struct NamedColor {
	const char *name;
	std::uint32_t rgb;
};

// The CSS colour keywords charts and meters actually use.
inline constexpr NamedColor kNamedColors[] = {
    {"black", 0x000000},       {"white", 0xffffff},     {"red", 0xff0000},       {"green", 0x008000},
    {"blue", 0x0000ff},        {"yellow", 0xffff00},    {"orange", 0xffa500},    {"purple", 0x800080},
    {"gray", 0x808080},        {"grey", 0x808080},      {"silver", 0xc0c0c0},    {"lime", 0x00ff00},
    {"aqua", 0x00ffff},        {"cyan", 0x00ffff},      {"magenta", 0xff00ff},   {"fuchsia", 0xff00ff},
    {"navy", 0x000080},        {"teal", 0x008080},      {"maroon", 0x800000},    {"olive", 0x808000},
    {"pink", 0xffc0cb},        {"brown", 0xa52a2a},     {"gold", 0xffd700},      {"indigo", 0x4b0082},
    {"violet", 0xee82ee},      {"crimson", 0xdc143c},   {"coral", 0xff7f50},     {"tomato", 0xff6347},
    {"salmon", 0xfa8072},      {"skyblue", 0x87ceeb},   {"steelblue", 0x4682b4}, {"darkgray", 0xa9a9a9},
    {"darkgrey", 0xa9a9a9},    {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969},     {"darkblue", 0x00008b},  {"darkgreen", 0x006400}, {"darkred", 0x8b0000},
    {"orangered", 0xff4500},   {"limegreen", 0x32cd32}, {"dodgerblue", 0x1e90ff}, {"deepskyblue", 0x00bfff},
    {"slategray", 0x708090},   {"slategrey", 0x708090}, {"whitesmoke", 0xf5f5f5}, {"turquoise", 0x40e0d0},
    {"tan", 0xd2b48c},         {"khaki", 0xf0e68c},     {"plum", 0xdda0dd},      {"orchid", 0xda70d6},
};
}  // namespace detail

// Parses a CSS colour: #rgb #rgba #rrggbb #rrggbbaa, rgb()/rgba() (numbers or
// percentages, comma or space syntax, `/ alpha`), hsl()/hsla(), the keywords
// above and `transparent`.
inline bool parseColor(const char *s, Color &out)
{
	if (!s) return false;
	while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
	if (*s == '#') {
		++s;
		int digits[8];
		int n = 0;
		while (n < 8 && detail::hexDigit(s[n]) >= 0) {
			digits[n] = detail::hexDigit(s[n]);
			++n;
		}
		if (detail::hexDigit(s[n]) >= 0) return false;
		if (n == 3 || n == 4) {
			out.r = static_cast<std::uint8_t>(digits[0] * 17);
			out.g = static_cast<std::uint8_t>(digits[1] * 17);
			out.b = static_cast<std::uint8_t>(digits[2] * 17);
			out.a = static_cast<std::uint8_t>(n == 4 ? digits[3] * 17 : 255);
			return true;
		}
		if (n == 6 || n == 8) {
			out.r = static_cast<std::uint8_t>(digits[0] * 16 + digits[1]);
			out.g = static_cast<std::uint8_t>(digits[2] * 16 + digits[3]);
			out.b = static_cast<std::uint8_t>(digits[4] * 16 + digits[5]);
			out.a = static_cast<std::uint8_t>(n == 8 ? digits[6] * 16 + digits[7] : 255);
			return true;
		}
		return false;
	}
	const bool rgb = detail::iequalsPrefix(s, "rgb");
	const bool hsl = detail::iequalsPrefix(s, "hsl");
	if (rgb || hsl) {
		const char *open = std::strchr(s, '(');
		if (!open) return false;
		float v[4] = {0, 0, 0, 1};
		bool pct[4] = {false, false, false, false};
		Scanner sc(open + 1);
		int n = 0;
		while (n < 4) {
			sc.skipSeparators();
			if (sc.peek() == '/') sc.take();
			if (!sc.number(v[n])) break;
			const char *r = sc.rest();
			if (*r == '%') { pct[n] = true; sc.advance(1); }
			else if (std::strncmp(r, "deg", 3) == 0) sc.advance(3);
			++n;
		}
		if (n < 3) return false;
		auto unit = [](float value, bool percent) { return std::clamp(percent ? value / 100.0f : value, 0.0f, 1.0f); };
		const float alpha = n == 4 ? unit(v[3], pct[3]) : 1.0f;
		if (rgb) {
			auto channel = [](float value, bool percent) {
				return static_cast<std::uint8_t>(std::lround(std::clamp(percent ? value * 2.55f : value, 0.0f, 255.0f)));
			};
			out.r = channel(v[0], pct[0]);
			out.g = channel(v[1], pct[1]);
			out.b = channel(v[2], pct[2]);
		} else {
			const float h = std::fmod(std::fmod(v[0], 360.0f) + 360.0f, 360.0f) / 360.0f;
			const float sat = std::clamp(v[1] / 100.0f, 0.0f, 1.0f);
			const float l = std::clamp(v[2] / 100.0f, 0.0f, 1.0f);
			float r = l, g = l, b = l;
			if (sat > 0) {
				const float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
				const float p = 2 * l - q;
				r = detail::hueToRgb(p, q, h + 1.0f / 3);
				g = detail::hueToRgb(p, q, h);
				b = detail::hueToRgb(p, q, h - 1.0f / 3);
			}
			out.r = static_cast<std::uint8_t>(std::lround(r * 255));
			out.g = static_cast<std::uint8_t>(std::lround(g * 255));
			out.b = static_cast<std::uint8_t>(std::lround(b * 255));
		}
		out.a = static_cast<std::uint8_t>(std::lround(alpha * 255));
		return true;
	}
	std::string word;
	for (const char *q = s; *q && !std::isspace(static_cast<unsigned char>(*q)); ++q)
		word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*q))));
	if (word == "transparent") {
		out = Color{0, 0, 0, 0};
		return true;
	}
	for (const auto &named : detail::kNamedColors) {
		if (word == named.name) {
			out.r = static_cast<std::uint8_t>(named.rgb >> 16);
			out.g = static_cast<std::uint8_t>(named.rgb >> 8);
			out.b = static_cast<std::uint8_t>(named.rgb);
			out.a = 255;
			return true;
		}
	}
	return false;
}

// A fill/stroke value: none | currentColor | <color>. Unparseable is Unset,
// so the property falls back to inheritance as an invalid declaration would.
inline Paint parsePaint(const char *s)
{
	Paint paint;
	if (!s) return paint;
	while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
	if (detail::iequalsPrefix(s, "none")) {
		paint.kind = PaintKind::None;
		return paint;
	}
	if (detail::iequalsPrefix(s, "currentcolor")) {
		paint.kind = PaintKind::CurrentColor;
		return paint;
	}
	// url(#id): a paint server (linearGradient) in the same svg.
	if (detail::iequalsPrefix(s, "url(")) {
		s += 4;
		while (*s == ' ' || *s == '\'' || *s == '"') ++s;
		if (*s == '#') ++s;
		std::size_t n = 0;
		while (s[n] && s[n] != ')' && s[n] != '\'' && s[n] != '"' && s[n] != ' ' && n + 1 < sizeof(paint.ref)) {
			paint.ref[n] = s[n];
			++n;
		}
		paint.ref[n] = '\0';
		if (n > 0) paint.kind = PaintKind::Url;
		return paint;
	}
	if (parseColor(s, paint.color)) paint.kind = paint.color.a == 0 ? PaintKind::None : PaintKind::Color;
	return paint;
}

// Paint packed into one style int (RareStyle): 0 unset, 1 none, 2 currentColor,
// otherwise (alpha7 << 24) | 0xRRGGBB with alpha7 in 1..127.
inline int encodePaint(const Paint &paint)
{
	switch (paint.kind) {
	case PaintKind::Unset: return 0;
	case PaintKind::None: return 1;
	case PaintKind::CurrentColor: return 2;
	case PaintKind::Url: return 0;  // attributes only; CSS refuses it (cssDeclarationValue)
	case PaintKind::Color: break;
	}
	const int alpha7 = (paint.color.a * 126 + 127) / 255 + 1;
	return (alpha7 << 24) | (paint.color.r << 16) | (paint.color.g << 8) | paint.color.b;
}

inline Paint decodePaint(int value)
{
	Paint paint;
	if (value == 0) return paint;
	if (value == 1) { paint.kind = PaintKind::None; return paint; }
	if (value == 2) { paint.kind = PaintKind::CurrentColor; return paint; }
	const int alpha7 = (value >> 24) & 0x7f;
	paint.kind = PaintKind::Color;
	paint.color.r = static_cast<std::uint8_t>(value >> 16);
	paint.color.g = static_cast<std::uint8_t>(value >> 8);
	paint.color.b = static_cast<std::uint8_t>(value);
	paint.color.a = static_cast<std::uint8_t>(alpha7 >= 127 ? 255 : ((alpha7 - 1) * 255 + 63) / 126);
	return paint;
}

// ---------------------------------------------------------------------------
// Transforms

// Parses a transform list (matrix translate scale rotate skewX skewY) and
// returns the composed matrix. A malformed list yields identity, as in SVG.
inline bool parseTransform(const char *s, Matrix &out)
{
	out = Matrix{};
	if (!s) return true;
	Scanner sc(s);
	Matrix result;
	while (!sc.atEnd()) {
		std::string name;
		while (std::isalpha(static_cast<unsigned char>(sc.peek()))) name.push_back(sc.take());
		sc.skipSpaces();
		if (sc.take() != '(') return false;
		float v[6] = {0, 0, 0, 0, 0, 0};
		int n = 0;
		while (n < 6 && sc.startsNumber() && sc.number(v[n])) ++n;
		sc.skipSeparators();
		if (sc.take() != ')') return false;
		Matrix m;
		if (name == "matrix" && n == 6) m = {v[0], v[1], v[2], v[3], v[4], v[5]};
		else if (name == "translate" && (n == 1 || n == 2)) m = Matrix::translate(v[0], n == 2 ? v[1] : 0);
		else if (name == "scale" && (n == 1 || n == 2)) m = Matrix::scaling(v[0], n == 2 ? v[1] : v[0]);
		else if (name == "rotate" && n == 1) m = Matrix::rotate(v[0]);
		else if (name == "rotate" && n == 3)
			m = Matrix::translate(v[1], v[2]) * Matrix::rotate(v[0]) * Matrix::translate(-v[1], -v[2]);
		else if (name == "skewX" && n == 1) m = {1, 0, std::tan(v[0] * 3.14159265358979f / 180.0f), 1, 0, 0};
		else if (name == "skewY" && n == 1) m = {1, std::tan(v[0] * 3.14159265358979f / 180.0f), 0, 1, 0, 0};
		else return false;
		result = result * m;
	}
	out = result;
	return true;
}

// viewBox="minx miny w h" with preserveAspectRatio, mapped onto a w x h box.
inline Matrix viewBoxTransform(const float vb[4], float boxW, float boxH, const char *preserveAspectRatio)
{
	if (vb[2] <= 0 || vb[3] <= 0) return Matrix{};
	float sx = boxW / vb[2];
	float sy = boxH / vb[3];
	std::string par = preserveAspectRatio ? preserveAspectRatio : "";
	if (par.find("none") != std::string::npos)
		return Matrix::scaling(sx, sy) * Matrix::translate(-vb[0], -vb[1]);
	const bool slice = par.find("slice") != std::string::npos;
	const float s = slice ? std::max(sx, sy) : std::min(sx, sy);
	float alignX = 0.5f, alignY = 0.5f;
	if (par.find("xMin") != std::string::npos) alignX = 0;
	if (par.find("xMax") != std::string::npos) alignX = 1;
	if (par.find("YMin") != std::string::npos) alignY = 0;
	if (par.find("YMax") != std::string::npos) alignY = 1;
	const float tx = (boxW - vb[2] * s) * alignX;
	const float ty = (boxH - vb[3] * s) * alignY;
	return Matrix::translate(tx, ty) * Matrix::scaling(s, s) * Matrix::translate(-vb[0], -vb[1]);
}

// ---------------------------------------------------------------------------
// Path construction and flattening

class PathBuilder {
public:
	// Curves flatten in device space: `toDevice` maps user units to pixels and
	// `tolerance` is the maximum deviation, in pixels, of a chord.
	PathBuilder(Path &out, const Matrix &toDevice, float tolerance = 0.2f)
	    : out_(out), m_(toDevice), tol_(std::max(0.01f, tolerance)) {}

	Point current() const { return cur_; }
	Point start() const { return start_; }

	void moveTo(Point p)
	{
		finishOpen();
		cur_ = start_ = p;
		out_.contours.push_back(Contour{m_.apply(p)});
		out_.closed.push_back(0);
		open_ = true;
	}
	void lineTo(Point p)
	{
		if (!open_) moveTo(cur_);
		emit(p);
		cur_ = p;
	}
	void cubicTo(Point c1, Point c2, Point p)
	{
		if (!open_) moveTo(cur_);
		const Point d0 = m_.apply(cur_), d1 = m_.apply(c1), d2 = m_.apply(c2), d3 = m_.apply(p);
		const float ddx = std::max(std::fabs(d0.x - 2 * d1.x + d2.x), std::fabs(d1.x - 2 * d2.x + d3.x));
		const float ddy = std::max(std::fabs(d0.y - 2 * d1.y + d2.y), std::fabs(d1.y - 2 * d2.y + d3.y));
		const float dd = std::sqrt(ddx * ddx + ddy * ddy);
		const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75f * dd / tol_))), 1, 128);
		const Point p0 = cur_;
		for (int i = 1; i <= n; ++i) {
			const float t = static_cast<float>(i) / n, u = 1 - t;
			const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
			emit({w0 * p0.x + w1 * c1.x + w2 * c2.x + w3 * p.x, w0 * p0.y + w1 * c1.y + w2 * c2.y + w3 * p.y});
		}
		cur_ = p;
	}
	void quadTo(Point c, Point p)
	{
		if (!open_) moveTo(cur_);
		const Point d0 = m_.apply(cur_), d1 = m_.apply(c), d2 = m_.apply(p);
		const float dd = std::hypot(d0.x - 2 * d1.x + d2.x, d0.y - 2 * d1.y + d2.y);
		const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.25f * dd / tol_))), 1, 128);
		const Point p0 = cur_;
		for (int i = 1; i <= n; ++i) {
			const float t = static_cast<float>(i) / n, u = 1 - t;
			emit({u * u * p0.x + 2 * u * t * c.x + t * t * p.x, u * u * p0.y + 2 * u * t * c.y + t * t * p.y});
		}
		cur_ = p;
	}
	// SVG elliptical arc (endpoint parameterization, implementation notes F.6.5).
	void arcTo(float rx, float ry, float rotationDeg, bool largeArc, bool sweep, Point p)
	{
		if (!open_) moveTo(cur_);
		const Point p0 = cur_;
		rx = std::fabs(rx);
		ry = std::fabs(ry);
		if ((p0.x == p.x && p0.y == p.y)) return;
		if (rx == 0 || ry == 0) {
			lineTo(p);
			return;
		}
		const float phi = rotationDeg * 3.14159265358979f / 180.0f;
		const float cs = std::cos(phi), sn = std::sin(phi);
		const float dx2 = (p0.x - p.x) / 2, dy2 = (p0.y - p.y) / 2;
		const float x1p = cs * dx2 + sn * dy2;
		const float y1p = -sn * dx2 + cs * dy2;
		const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
		if (lambda > 1) {
			const float s = std::sqrt(lambda);
			rx *= s;
			ry *= s;
		}
		// In double: near a half circle `num` is the difference of two nearly
		// equal terms, and in float the centre then jitters by hundredths of a
		// pixel as the end moves, so every grid point of an arc that only grew
		// would move and its whole box would be damaged.
		const double rx2 = static_cast<double>(rx) * rx, ry2 = static_cast<double>(ry) * ry;
		const double x1p2 = static_cast<double>(x1p) * x1p, y1p2 = static_cast<double>(y1p) * y1p;
		const double num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
		const double den = rx2 * y1p2 + ry2 * x1p2;
		float coef = den > 0 ? static_cast<float>(std::sqrt(std::max(0.0, num / den))) : 0.0f;
		if (largeArc == sweep) coef = -coef;
		const float cxp = coef * rx * y1p / ry;
		const float cyp = -coef * ry * x1p / rx;
		const float cx = cs * cxp - sn * cyp + (p0.x + p.x) / 2;
		const float cy = sn * cxp + cs * cyp + (p0.y + p.y) / 2;
		auto angle = [](float ux, float uy, float vx, float vy) {
			return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
		};
		const float theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
		float dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
		const float twoPi = 2 * 3.14159265358979f;
		if (!sweep && dtheta > 0) dtheta -= twoPi;
		else if (sweep && dtheta < 0) dtheta += twoPi;
		const float radius = std::max(rx, ry) * m_.scale();
		// The arc's points sit on a fixed grid of angles, not an even split of
		// its own sweep: two arcs that share a start and differ only in where
		// they end (a gauge growing by a degree) flatten to the same points up
		// to the old end, so the scene rebuild damages only the changed tip.
		// The grid step is 0.8 of the tolerance step because the ends skip a
		// grid angle closer than a quarter step, stretching that chord.
		const float step = radius > tol_ ? 2 * std::acos(std::max(-1.0f, 1 - tol_ / radius)) : 3.14159265358979f / 2;
		const int perTurn = std::clamp(static_cast<int>(std::ceil(twoPi / std::max(0.8f * step, 0.01f))), 4, 2048);
		const float grid = twoPi / static_cast<float>(perTurn);
		const float theta2 = theta1 + dtheta;
		const float margin = 0.25f * grid;
		auto point = [&](float t) {
			const float ex = rx * std::cos(t), ey = ry * std::sin(t);
			emit({cs * ex - sn * ey + cx, sn * ex + cs * ey + cy});
		};
		if (dtheta > 0) {
			for (int k = static_cast<int>(std::floor(theta1 / grid)) + 1, guard = 0; guard <= perTurn + 1; ++k, ++guard) {
				const float t = static_cast<float>(k) * grid;
				if (t >= theta2 - margin) break;
				if (t > theta1 + margin) point(t);
			}
		} else {
			for (int k = static_cast<int>(std::ceil(theta1 / grid)) - 1, guard = 0; guard <= perTurn + 1; --k, ++guard) {
				const float t = static_cast<float>(k) * grid;
				if (t <= theta2 + margin) break;
				if (t < theta1 - margin) point(t);
			}
		}
		emit(p);
		cur_ = p;
	}
	void close()
	{
		if (!open_) return;
		out_.closed.back() = 1;
		open_ = false;
		cur_ = start_;
	}
	void finish() { finishOpen(); }

private:
	void emit(Point userPoint)
	{
		const Point d = m_.apply(userPoint);
		Contour &c = out_.contours.back();
		const Point &last = c.back();
		if (std::fabs(last.x - d.x) < 1e-4f && std::fabs(last.y - d.y) < 1e-4f) return;
		c.push_back(d);
	}
	void finishOpen()
	{
		// A lone moveTo draws nothing and must not leave a one-point contour.
		if (!out_.contours.empty() && out_.contours.back().size() < 2 && !out_.closed.back()) {
			out_.contours.pop_back();
			out_.closed.pop_back();
		}
		open_ = false;
	}

	Path &out_;
	Matrix m_;
	float tol_;
	Point cur_, start_;
	bool open_ = false;
};

// Path data. Returns false at the first error; everything before it stays
// drawn, as the SVG error-handling rules require.
inline bool parsePathData(const char *d, PathBuilder &b)
{
	Scanner sc(d);
	char cmd = 0;
	Point lastCtrl;
	char prev = 0;
	bool ok = true;
	while (!sc.atEnd()) {
		const char next = sc.peek();
		if (std::isalpha(static_cast<unsigned char>(next))) {
			cmd = sc.take();
		} else if (!cmd || cmd == 'Z' || cmd == 'z') {
			ok = false;
			break;
		}
		const bool rel = std::islower(static_cast<unsigned char>(cmd));
		const Point c = b.current();
		auto abs = [&](float x, float y) { return rel ? Point{c.x + x, c.y + y} : Point{x, y}; };
		float v[7];
		switch (std::toupper(static_cast<unsigned char>(cmd))) {
		case 'M':
			if (!sc.number(v[0]) || !sc.number(v[1])) { ok = false; break; }
			b.moveTo(abs(v[0], v[1]));
			cmd = rel ? 'l' : 'L';  // subsequent pairs are implicit lineTos
			break;
		case 'L':
			if (!sc.number(v[0]) || !sc.number(v[1])) { ok = false; break; }
			b.lineTo(abs(v[0], v[1]));
			break;
		case 'H':
			if (!sc.number(v[0])) { ok = false; break; }
			b.lineTo({rel ? c.x + v[0] : v[0], c.y});
			break;
		case 'V':
			if (!sc.number(v[0])) { ok = false; break; }
			b.lineTo({c.x, rel ? c.y + v[0] : v[0]});
			break;
		case 'C':
			if (!sc.number(v[0]) || !sc.number(v[1]) || !sc.number(v[2]) || !sc.number(v[3]) || !sc.number(v[4]) ||
			    !sc.number(v[5])) { ok = false; break; }
			lastCtrl = abs(v[2], v[3]);
			b.cubicTo(abs(v[0], v[1]), lastCtrl, abs(v[4], v[5]));
			break;
		case 'S': {
			if (!sc.number(v[0]) || !sc.number(v[1]) || !sc.number(v[2]) || !sc.number(v[3])) { ok = false; break; }
			const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(prev)));
			const Point c1 = (up == 'C' || up == 'S') ? Point{2 * c.x - lastCtrl.x, 2 * c.y - lastCtrl.y} : c;
			lastCtrl = abs(v[0], v[1]);
			b.cubicTo(c1, lastCtrl, abs(v[2], v[3]));
			break;
		}
		case 'Q':
			if (!sc.number(v[0]) || !sc.number(v[1]) || !sc.number(v[2]) || !sc.number(v[3])) { ok = false; break; }
			lastCtrl = abs(v[0], v[1]);
			b.quadTo(lastCtrl, abs(v[2], v[3]));
			break;
		case 'T': {
			if (!sc.number(v[0]) || !sc.number(v[1])) { ok = false; break; }
			const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(prev)));
			lastCtrl = (up == 'Q' || up == 'T') ? Point{2 * c.x - lastCtrl.x, 2 * c.y - lastCtrl.y} : c;
			b.quadTo(lastCtrl, abs(v[0], v[1]));
			break;
		}
		case 'A': {
			bool large = false, sweep = false;
			if (!sc.number(v[0]) || !sc.number(v[1]) || !sc.number(v[2]) || !sc.flag(large) || !sc.flag(sweep) ||
			    !sc.number(v[3]) || !sc.number(v[4])) { ok = false; break; }
			b.arcTo(v[0], v[1], v[2], large, sweep, abs(v[3], v[4]));
			break;
		}
		case 'Z':
			b.close();
			break;
		default:
			ok = false;
			break;
		}
		if (!ok) break;
		prev = cmd;
	}
	b.finish();
	return ok;
}

// polyline/polygon `points`. An odd trailing coordinate is ignored.
inline void addPoints(PathBuilder &b, const char *points, bool close)
{
	Scanner sc(points);
	float x = 0, y = 0;
	bool first = true;
	while (sc.number(x) && sc.number(y)) {
		if (first) b.moveTo({x, y});
		else b.lineTo({x, y});
		first = false;
	}
	if (!first && close) b.close();
	b.finish();
}

inline void addEllipse(PathBuilder &b, float cx, float cy, float rx, float ry)
{
	if (rx <= 0 || ry <= 0) return;
	b.moveTo({cx + rx, cy});
	b.arcTo(rx, ry, 0, false, true, {cx - rx, cy});
	b.arcTo(rx, ry, 0, false, true, {cx + rx, cy});
	b.close();
	b.finish();
}

inline void addRect(PathBuilder &b, float x, float y, float w, float h, float rx, float ry)
{
	if (w <= 0 || h <= 0) return;
	rx = std::min(std::max(rx, 0.0f), w / 2);
	ry = std::min(std::max(ry, 0.0f), h / 2);
	if (rx <= 0 || ry <= 0) {
		b.moveTo({x, y});
		b.lineTo({x + w, y});
		b.lineTo({x + w, y + h});
		b.lineTo({x, y + h});
		b.close();
	} else {
		b.moveTo({x + rx, y});
		b.lineTo({x + w - rx, y});
		b.arcTo(rx, ry, 0, false, true, {x + w, y + ry});
		b.lineTo({x + w, y + h - ry});
		b.arcTo(rx, ry, 0, false, true, {x + w - rx, y + h});
		b.lineTo({x + rx, y + h});
		b.arcTo(rx, ry, 0, false, true, {x, y + h - ry});
		b.lineTo({x, y + ry});
		b.arcTo(rx, ry, 0, false, true, {x + rx, y});
		b.close();
	}
	b.finish();
}

// ---------------------------------------------------------------------------
// Stroking

enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

struct StrokeStyle {
	float width = 1.0f;  // device pixels
	LineCap cap = LineCap::Butt;
	LineJoin join = LineJoin::Miter;
	float miterLimit = 4.0f;
};

namespace detail {
// Every stroke piece winds the same way, so overlapping pieces add instead of
// cancelling and the nonzero fill of the set is exactly their union. A piece
// reaches the sink as points plus `reversed`: when set, the piece is to be
// read backwards (its edges' directions flipped), which is how it would wind
// had it been stored reversed. Pieces never touch the heap: a stroke is
// hundreds of them, and an ESP32 allocation is a microsecond or two.
template <typename Sink>
inline void pushPiece(Sink &sink, const Point *piece, int n)
{
	if (n < 3) return;
	float area = 0;
	for (int i = 0; i < n; ++i) {
		const Point &p = piece[i], &q = piece[(i + 1) % n];
		area += p.x * q.y - q.x * p.y;
	}
	area *= 0.5f;
	if (std::fabs(area) < 1e-6f) return;
	sink(piece, n, area < 0);
}
template <typename Sink>
inline void pushPiece(Sink &sink, std::initializer_list<Point> piece)
{
	pushPiece(sink, piece.begin(), static_cast<int>(piece.size()));
}

template <typename Sink>
inline void pushDisc(Sink &sink, Point c, float r)
{
	const int n = std::clamp(static_cast<int>(std::ceil(r * 1.6f)) + 6, 8, 64);
	Point disc[64];
	for (int i = 0; i < n; ++i) {
		const float t = 2 * 3.14159265358979f * i / n;
		disc[i] = {c.x + r * std::cos(t), c.y + r * std::sin(t)};
	}
	pushPiece(sink, disc, n);
}
// The round join's outer wedge: the pie slice from o0 to o1 around p, with
// just enough arc points to keep each chord within 0.2 px of the circle. (A
// whole disc per vertex costs ~20 points where a flattened curve's joins need
// one or two.)
template <typename Sink>
inline void pushWedge(Sink &sink, Point p, Point o0, float r, float turn)
{
	const float step = r > 0.2f ? 2 * std::acos(1 - 0.2f / r) : 1.5707963f;
	const int n = std::clamp(static_cast<int>(std::ceil(std::fabs(turn) / step)), 1, 64);
	Point wedge[66];
	int count = 0;
	wedge[count++] = p;
	const float a0 = std::atan2(o0.y - p.y, o0.x - p.x);
	// o1 is o0 rotated about p by the path's turning angle (both offsets are
	// the segment normals scaled by the same side), so the arc sweeps `turn`.
	const float sweep = turn;
	for (int i = 0; i <= n; ++i) {
		const float t = a0 + sweep * i / n;
		wedge[count++] = {p.x + r * std::cos(t), p.y + r * std::sin(t)};
	}
	pushPiece(sink, wedge, count);
}
}  // namespace detail

// Outlines one device-space polyline as polygons to fill with nonzero, each
// handed to `sink(points, count, reversed)`.
//
// The outline is one polygon per open subpath (and two, outer and inner, per
// closed one): the left offset forward, the end cap, the right offset back,
// the start cap -- the construction Skia's stroker fills with nonzero. At a
// joint the outer side gets the join's own points (miter tip, round arc, or
// nothing for a bevel) and the inner side pivots through the vertex itself,
// which keeps the winding positive where the two offsets overlap. Compared
// with a quad per segment plus a piece per joint and a disc per cap, this
// hands the rasterizer about half the edges (no quad ends, no overlapping
// pieces) and the builder a quarter, and its arcs step by a fixed rotation:
// one cos/sin per stroke instead of two per arc point.
namespace detail {
struct ArcStep {
	float step = 1.5707963f;
	explicit ArcStep(float r)
	{
		// Each chord within 0.2 px of the circle, and never coarser than the
		// disc pushDisc draws (small radii need the extra points for area).
		step = r > 0.2f ? 2 * std::acos(1 - 0.2f / r) : 1.5707963f;
		const int disc = std::clamp(static_cast<int>(std::ceil(r * 1.6f)) + 6, 8, 64);
		step = std::min(step, 6.2831853f / static_cast<float>(disc));
	}
};
// Points strictly between p+u and p+u rotated by `sweep` (radians, signed),
// appended to `out`. |u| is the radius.
template <typename Out>
inline void arcPoints(Out &out, Point p, Point u, float sweep, float step)
{
	const int n = std::clamp(static_cast<int>(std::ceil(std::fabs(sweep) / step)), 1, 64);
	if (n < 2) return;
	const float d = sweep / n;
	const float c = std::cos(d), s = std::sin(d);
	float x = u.x, y = u.y;
	for (int i = 1; i < n; ++i) {
		const float nx = x * c - y * s;
		y = x * s + y * c;
		x = nx;
		out.push_back({p.x + x, p.y + y});
	}
}
inline float polygonArea2(const Point *p, std::size_t n)
{
	float area = 0;
	for (std::size_t i = 0; i < n; ++i) {
		const Point &a = p[i], &b = p[i + 1 < n ? i + 1 : 0];
		area += a.x * b.y - b.x * a.y;
	}
	return area;
}
}  // namespace detail

template <typename Sink>
inline void strokeContour(const Contour &in, bool closed, const StrokeStyle &style, Sink &&sink)
{
	const float hw = style.width * 0.5f;
	if (hw <= 0 || in.empty()) return;
	ScratchVector<Point> pts;
	pts.reserve(in.size());
	for (const Point &p : in) {
		if (!pts.empty()) {
			const float dx = p.x - pts.back().x, dy = p.y - pts.back().y;
			if (dx * dx + dy * dy <= 1e-8f) continue;
		}
		pts.push_back(p);
	}
	if (closed && pts.size() > 2) {
		const float dx = pts.front().x - pts.back().x, dy = pts.front().y - pts.back().y;
		if (dx * dx + dy * dy <= 1e-8f) pts.pop_back();
	}
	const std::size_t n = pts.size();
	if (n == 1) {
		// Zero-length subpath: round and square caps still paint a dot.
		if (style.cap == LineCap::Round) detail::pushDisc(sink, pts[0], hw);
		else if (style.cap == LineCap::Square)
			detail::pushPiece(sink, {{pts[0].x - hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y - hw},
			                         {pts[0].x + hw, pts[0].y + hw}, {pts[0].x - hw, pts[0].y + hw}});
		return;
	}
	const std::size_t segments = closed ? n : n - 1;
	// Each segment's left normal, scaled to the half width: solved once, read
	// by both sides and by the joins on both its ends.
	ScratchVector<Point> normals(segments);
	ScratchVector<float> lengths(segments);
	for (std::size_t i = 0; i < segments; ++i) {
		const Point &a = pts[i], &b = pts[(i + 1) % n];
		const float dx = b.x - a.x, dy = b.y - a.y;
		const float len = std::sqrt(dx * dx + dy * dy);
		const float k = hw / len;
		normals[i] = Point{-dy * k, dx * k};
		lengths[i] = len;
	}
	const bool roundArcs = style.cap == LineCap::Round || style.join == LineJoin::Round;
	const float arcStep = roundArcs ? detail::ArcStep(hw).step : 1.5707963f;
	ScratchVector<Point> out;
	out.reserve(segments * 4 + 8);
	// The joint at vertex p between offsets a (arriving) and b (leaving), on
	// the side whose offsets these are.
	auto joint = [&](Point p, Point a, Point b, float cross, float dot, float lenA, float lenB) {
		if (dot > 0 && std::fabs(cross) < 1e-4f * dot) {  // straight on
			out.push_back({p.x + b.x, p.y + b.y});
			return;
		}
		// `cross` is the turn as seen from this side's offsets: negative when
		// the path turns away from them, where they fan apart.
		const bool outer = cross <= 0;
		const float side = a.x * b.y - a.y * b.x;
		// Where the two offset lines cross, when that point alone is the exact
		// outline: on the outer side a miter within the limit (the offset
		// points p+a and p+b lie on the lines into and out of the tip, so they
		// add only collinear edges), and on the inner side a crossing within
		// half of both segments (each segment may be cut from both ends). A
		// flattened curve then costs two outline points per vertex, not six.
		if (dot > 0 && (outer ? style.join == LineJoin::Miter : true)) {
			const float cosTurn = std::clamp(dot / (hw * hw), -1.0f, 1.0f);
			const float cosHalf = std::sqrt((1 + cosTurn) * 0.5f);
			const float sinHalf = std::sqrt(std::max(0.0f, (1 - cosTurn) * 0.5f));
			const bool fits = outer ? 1.0f / cosHalf <= style.miterLimit
			                        : hw * sinHalf <= cosHalf * 0.5f * std::min(lenA, lenB);
			if (cosHalf > 1e-4f && fits) {
				const Point mid{a.x + b.x, a.y + b.y};
				const float k = hw / (cosHalf * std::sqrt(mid.x * mid.x + mid.y * mid.y));
				out.push_back({p.x + mid.x * k, p.y + mid.y * k});
				return;
			}
		}
		out.push_back({p.x + a.x, p.y + a.y});
		if (!outer) {
			out.push_back(p);
		} else if (style.join == LineJoin::Round) {
			// `a` is the left normal of the arriving direction, so a full
			// reversal (side 0) sweeps through that direction: -pi.
			const float turn = std::fabs(cross) < 1e-4f * hw * hw ? -3.14159265f : std::atan2(side, a.x * b.x + a.y * b.y);
			if (hw * std::fabs(turn) >= 1.0f) detail::arcPoints(out, p, a, turn, arcStep);
		} else if (style.join == LineJoin::Miter) {
			const float cosTurn = std::clamp(dot / (hw * hw), -1.0f, 1.0f);
			const float cosHalf = std::sqrt((1 + cosTurn) * 0.5f);
			if (cosHalf > 1e-4f && 1.0f / cosHalf <= style.miterLimit) {
				const Point mid{a.x + b.x, a.y + b.y};
				const float k = hw / (cosHalf * std::sqrt(mid.x * mid.x + mid.y * mid.y));
				out.push_back({p.x + mid.x * k, p.y + mid.y * k});
			}
		}
		out.push_back({p.x + b.x, p.y + b.y});
	};
	auto cap = [&](Point p, Point u) {  // from p+u round to p-u, through the forward side
		if (style.cap == LineCap::Round) {
			out.push_back({p.x + u.x, p.y + u.y});
			detail::arcPoints(out, p, u, -3.14159265f, arcStep);
			out.push_back({p.x - u.x, p.y - u.y});
		} else if (style.cap == LineCap::Square) {
			const Point f{u.y, -u.x};  // forward, half width long
			out.push_back({p.x + u.x + f.x, p.y + u.y + f.y});
			out.push_back({p.x - u.x + f.x, p.y - u.y + f.y});
		} else {
			out.push_back({p.x + u.x, p.y + u.y});
			out.push_back({p.x - u.x, p.y - u.y});
		}
	};
	auto turnAt = [&](std::size_t prevSeg, std::size_t nextSeg, float &cross, float &dot) {
		const Point n0 = normals[prevSeg], n1 = normals[nextSeg];
		cross = n0.x * n1.y - n0.y * n1.x;
		dot = n0.x * n1.x + n0.y * n1.y;
	};
	if (!closed) {
		// Left side forward.
		out.push_back({pts[0].x + normals[0].x, pts[0].y + normals[0].y});
		for (std::size_t v = 1; v < n - 1; ++v) {
			float cross, dot;
			turnAt(v - 1, v, cross, dot);
			joint(pts[v], normals[v - 1], normals[v], cross, dot, lengths[v - 1], lengths[v]);
		}
		// End cap, right side back, start cap.
		cap(pts[n - 1], normals[segments - 1]);
		for (std::size_t v = n - 2; v >= 1; --v) {
			float cross, dot;
			turnAt(v - 1, v, cross, dot);
			const Point a{-normals[v].x, -normals[v].y}, b{-normals[v - 1].x, -normals[v - 1].y};
			joint(pts[v], a, b, -cross, dot, lengths[v], lengths[v - 1]);
		}
		cap(pts[0], Point{-normals[0].x, -normals[0].y});
		if (style.cap != LineCap::Square) out.pop_back();  // the start cap ends where the outline began
		const float area = detail::polygonArea2(out.data(), out.size());
		if (std::fabs(area) > 2e-6f) sink(out.data(), static_cast<int>(out.size()), area < 0);
		return;
	}
	// Closed: the left loop forward and the right loop backward, wound
	// oppositely, so nonzero fills only the band between them.
	for (std::size_t v = 0; v < n; ++v) {
		float cross, dot;
		const std::size_t prev = (v + segments - 1) % segments;
		turnAt(prev, v, cross, dot);
		joint(pts[v], normals[prev], normals[v], cross, dot, lengths[prev], lengths[v]);
	}
	const std::size_t leftCount = out.size();
	for (std::size_t k = 0; k < n; ++k) {
		const std::size_t v = n - 1 - k;
		float cross, dot;
		const std::size_t prev = (v + segments - 1) % segments;
		turnAt(prev, v, cross, dot);
		const Point a{-normals[v].x, -normals[v].y}, b{-normals[prev].x, -normals[prev].y};
		joint(pts[v], a, b, -cross, dot, lengths[v], lengths[prev]);
	}
	const float area = detail::polygonArea2(out.data(), leftCount) +
	                   detail::polygonArea2(out.data() + leftCount, out.size() - leftCount);
	if (std::fabs(area) <= 2e-6f) return;
	sink(out.data(), static_cast<int>(leftCount), area < 0);
	sink(out.data() + leftCount, static_cast<int>(out.size() - leftCount), area < 0);
}

// The stroke outline as polygons (each wound the same way).
inline void strokeContour(const Contour &in, bool closed, const StrokeStyle &style, std::vector<Contour> &out)
{
	strokeContour(in, closed, style, [&out](const Point *p, int n, bool reversed) {
		Contour piece(p, p + n);
		if (reversed) std::reverse(piece.begin(), piece.end());
		out.push_back(std::move(piece));
	});
}

inline void strokePath(const Path &path, const StrokeStyle &style, std::vector<Contour> &out)
{
	for (std::size_t i = 0; i < path.contours.size(); ++i) strokeContour(path.contours[i], path.closed[i] != 0, style, out);
}

// ---------------------------------------------------------------------------
// Bounds and hit testing

struct Bounds {
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	bool empty() const { return x0 > x1 || y0 > y1; }
	void add(Point p)
	{
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	void add(const std::vector<Contour> &contours)
	{
		for (const auto &c : contours)
			for (const Point &p : c) add(p);
	}
};

// Winding number of `p` against closed contours (open ones close implicitly).
inline int windingNumber(const std::vector<Contour> &contours, Point p)
{
	int wn = 0;
	for (const auto &c : contours) {
		const std::size_t n = c.size();
		if (n < 2) continue;
		for (std::size_t i = 0; i < n; ++i) {
			const Point &a = c[i], &b = c[(i + 1) % n];
			const float isLeft = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
			if (a.y <= p.y) {
				if (b.y > p.y && isLeft > 0) ++wn;
			} else if (b.y <= p.y && isLeft < 0) {
				--wn;
			}
		}
	}
	return wn;
}

inline bool insideFill(const std::vector<Contour> &contours, Point p, bool evenOdd)
{
	const int wn = windingNumber(contours, p);
	return evenOdd ? (wn & 1) != 0 : wn != 0;
}

inline float distanceToPath(const Path &path, Point p)
{
	float best = 1e30f;
	for (std::size_t ci = 0; ci < path.contours.size(); ++ci) {
		const Contour &c = path.contours[ci];
		const std::size_t n = c.size();
		const std::size_t segs = path.closed[ci] ? n : (n ? n - 1 : 0);
		for (std::size_t i = 0; i < segs; ++i) {
			const Point &a = c[i], &b = c[(i + 1) % n];
			const float dx = b.x - a.x, dy = b.y - a.y;
			const float len2 = dx * dx + dy * dy;
			float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
			t = std::clamp(t, 0.0f, 1.0f);
			best = std::min(best, std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y));
		}
		if (n == 1) best = std::min(best, std::hypot(c[0].x - p.x, c[0].y - p.y));
	}
	return best;
}

// ---------------------------------------------------------------------------
// Anti-aliased coverage rasterizer

// Exact-area coverage over a pixel rectangle, built from signed edge
// contributions and resolved by a running sum per row (the font-rs scheme).
// Edges are clipped to the rectangle, so it can rasterize any horizontal strip
// of a larger shape with no seams between strips.
// A shape's edges, flat and sorted by top y: built once per scene build, then
// every paint and every strip walks one contiguous array.
// `a` is the top end and `b` the bottom; `dir` is the winding the contour
// gave it (+1 drawn downwards, -1 upwards). The slopes are solved once here:
// a tall edge crosses many strips, and each strip re-clips it.
struct Edge {
	Point a, b;
	float top, bottom;
	float dxdy, dydx;  // dydx is 0 for a vertical edge (never used then)
	float dir;
};
using EdgeList = std::vector<Edge, ExternalAllocator<Edge>>;

// Collects polygon edges and writes them out sorted by top. Edges are built
// in internal scratch and ordered by sorting 8-byte keys (top, index), then
// written out once, in order: sorting the 36-byte edges themselves in place,
// in PSRAM, cost ~700 cycles per edge on an ESP32-S3.
class EdgeBuilder {
public:
	void reserve(std::size_t n)
	{
		built_.reserve(n);
		keys_.reserve(n);
	}
	// One closed polygon; `reversed` reads it backwards (flips every edge).
	void add(const Point *p, int n, bool reversed = false)
	{
		for (int i = 0; i < n; ++i) {
			Point a = p[i], b = p[i + 1 < n ? i + 1 : 0];
			if (a.y == b.y) continue;
			float dir = reversed ? -1.0f : 1.0f;
			if (a.y > b.y) std::swap(a, b), dir = -dir;
			const float dx = b.x - a.x, dy = b.y - a.y;
			// The float's bits, flipped so unsigned order is numeric order.
			std::uint32_t bits;
			std::memcpy(&bits, &a.y, sizeof bits);
			bits ^= (bits >> 31) ? 0xffffffffu : 0x80000000u;
			keys_.push_back(static_cast<std::uint64_t>(bits) << 32 | built_.size());
			built_.push_back({a, b, a.y, b.y, dx / dy, dx != 0.0f ? dy / dx : 0.0f, dir});
		}
	}
	void finish(EdgeList &edges)
	{
		std::sort(keys_.begin(), keys_.end());
		edges.resize(built_.size());
		for (std::size_t i = 0; i < keys_.size(); ++i) edges[i] = built_[static_cast<std::uint32_t>(keys_[i])];
	}

private:
	ScratchVector<Edge> built_;
	ScratchVector<std::uint64_t> keys_;
};

inline void buildEdges(const std::vector<Contour> &contours, EdgeList &edges)
{
	edges.clear();
	std::size_t total = 0;
	for (const auto &c : contours)
		if (c.size() >= 2) total += c.size();
	if (!total) return;
	EdgeBuilder builder;
	builder.reserve(total);
	for (const auto &c : contours)
		if (c.size() >= 2) builder.add(c.data(), static_cast<int>(c.size()));
	builder.finish(edges);
}

// A path's stroke outline straight to edges (what strokePath + buildEdges
// make, without the per-piece polygons), and the outline's bounds.
inline void strokePathEdges(const Path &path, const StrokeStyle &style, EdgeList &edges, Bounds &bounds)
{
	edges.clear();
	EdgeBuilder builder;
	std::size_t points = 0;
	for (const auto &c : path.contours) points += c.size();
	builder.reserve(points * 8 + 16);
	auto sink = [&](const Point *p, int n, bool reversed) {
		for (int i = 0; i < n; ++i) bounds.add(p[i]);
		builder.add(p, n, reversed);
	};
	for (std::size_t i = 0; i < path.contours.size(); ++i) strokeContour(path.contours[i], path.closed[i] != 0, style, sink);
	builder.finish(edges);
}

class Rasterizer {
public:
	// The accumulator stays all-zero between uses: row() clears exactly the
	// cells each row touched, so a strip never re-zeroes its whole area and
	// work scales with the ink, not the bounding box.
	GEA_SVG_HOT void reset(int x0, int y0, int width, int height)
	{
		x0_ = x0;
		y0_ = y0;
		w_ = std::max(0, width);
		h_ = std::max(0, height);
		stride_ = w_ + 2;
		wpr_ = (stride_ + 31) >> 5;
		const std::size_t cells = static_cast<std::size_t>(stride_) * static_cast<std::size_t>(h_);
		growInternalZeroed(acc_, cells, kReserveCells, accRetry_);
		const std::size_t words = static_cast<std::size_t>(wpr_) * static_cast<std::size_t>(h_);
		growInternalZeroed(bits_, words, kReserveWords, bitsRetry_);
	}
	int x() const { return x0_; }
	int y() const { return y0_; }
	int width() const { return w_; }
	int height() const { return h_; }

	// Rasterizes from a prebuilt edge list (not copied; must outlive the use).
	void useEdges(const EdgeList &edges) { edges_ = &edges; }
	// Builds this rasterizer's own edge list from contours.
	void setEdges(const std::vector<Contour> &contours)
	{
		buildEdges(contours, ownEdges_);
		edges_ = &ownEdges_;
	}

	// Adds the edges that reach the current strip.
	void addStoredEdges()
	{
		if (!edges_) return;
		const float top = static_cast<float>(y0_), bottom = static_cast<float>(y0_ + h_);
		for (const Edge &e : *edges_) {
			if (e.top >= bottom) break;
			if (e.bottom <= top) continue;
			addStoredEdge(e);
		}
	}

	// Strip-by-strip walk of the stored edges: each strip adds only the edges
	// still open (kept in an active list) plus the ones it newly reaches, so a
	// tall shape cut into many short strips never re-scans its whole list.
	void beginEdgeWalk()
	{
		nextEdge_ = 0;
		active_.clear();
	}
	GEA_SVG_HOT void addActiveEdges()
	{
		if (!edges_) return;
		const float top = static_cast<float>(y0_), bottom = static_cast<float>(y0_ + h_);
		const EdgeList &edges = *edges_;
		std::size_t kept = 0;
		for (std::size_t i = 0; i < active_.size(); ++i)
			if (edges[active_[i]].bottom > top) active_[kept++] = active_[i];
		active_.resize(kept);
		while (nextEdge_ < edges.size() && edges[nextEdge_].top < bottom) {
			if (edges[nextEdge_].bottom > top) active_.push_back(static_cast<std::uint32_t>(nextEdge_));
			++nextEdge_;
		}
		for (std::uint32_t i : active_) addStoredEdge(edges[i]);
	}

	// addEdge for a stored edge: the same clipping, from its solved slopes.
	GEA_SVG_HOT void addStoredEdge(const Edge &e)
	{
		if (w_ <= 0 || h_ <= 0) return;
		const float W = static_cast<float>(w_), H = static_cast<float>(h_);
		Point a{e.a.x - x0_, e.a.y - y0_}, b{e.b.x - x0_, e.b.y - y0_};
		if (b.y <= 0 || a.y >= H) return;
		if (a.y < 0) { a.x -= e.dxdy * a.y; a.y = 0; }
		if (b.y > H) { b.x = a.x + e.dxdy * (H - a.y); b.y = H; }
		if (b.y <= a.y) return;
		const float lo = std::min(a.x, b.x), hi = std::max(a.x, b.x);
		if (lo >= 0 && hi <= W) {
			line(a, b, e.dir, e.dxdy);
			return;
		}
		Point pts[4];
		int count = 0;
		pts[count++] = a;
		float ys[2];
		int nt = 0;
		if (lo < 0 && hi > 0) ys[nt++] = a.y + (0 - a.x) * e.dydx;
		if (lo < W && hi > W) ys[nt++] = a.y + (W - a.x) * e.dydx;
		if (nt == 2 && ys[0] > ys[1]) std::swap(ys[0], ys[1]);
		for (int i = 0; i < nt; ++i) {
			const float y = std::clamp(ys[i], a.y, b.y);
			pts[count++] = {a.x + e.dxdy * (y - a.y), y};
		}
		pts[count++] = b;
		for (int i = 0; i + 1 < count; ++i) {
			Point p = pts[i], q = pts[i + 1];
			// A piece beyond a side column is flattened onto it, vertical.
			const float mid = (p.x + q.x) * 0.5f;
			const bool outside = mid < 0 || mid > W;
			p.x = std::clamp(p.x, 0.0f, W);
			q.x = std::clamp(q.x, 0.0f, W);
			line(p, q, e.dir, outside ? 0.0f : e.dxdy);
		}
	}

	void addContours(const std::vector<Contour> &contours, float dx = 0, float dy = 0)
	{
		for (const auto &c : contours) {
			const std::size_t n = c.size();
			if (n < 2) continue;
			for (std::size_t i = 0; i < n; ++i) {
				const Point &a = c[i], &b = c[(i + 1) % n];
				addEdge({a.x + dx, a.y + dy}, {b.x + dx, b.y + dy});
			}
		}
	}

	// One edge in device pixels.
	void addEdge(Point a, Point b)
	{
		if (w_ <= 0 || h_ <= 0) return;
		a.x -= x0_; a.y -= y0_;
		b.x -= x0_; b.y -= y0_;
		const float W = static_cast<float>(w_), H = static_cast<float>(h_);
		if ((a.y <= 0 && b.y <= 0) || (a.y >= H && b.y >= H) || a.y == b.y) return;
		// Clip to the strip's rows.
		if (a.y > b.y) std::swap(a, b), dirFlip_ = true;
		else dirFlip_ = false;
		if (a.y < 0) { a.x += (b.x - a.x) * (0 - a.y) / (b.y - a.y); a.y = 0; }
		if (b.y > H) { b.x = a.x + (b.x - a.x) * (H - a.y) / (b.y - a.y); b.y = H; }
		// Split at the left/right columns: the part outside becomes a vertical
		// edge on the boundary, which keeps every row's winding exact.
		Point pts[4];
		int count = 0;
		pts[count++] = a;
		const float lo = std::min(a.x, b.x), hi = std::max(a.x, b.x);
		float ts[2];
		int nt = 0;
		if (lo < 0 && hi > 0) ts[nt++] = (0 - a.x) / (b.x - a.x);
		if (lo < W && hi > W) ts[nt++] = (W - a.x) / (b.x - a.x);
		if (nt == 2 && ts[0] > ts[1]) std::swap(ts[0], ts[1]);
		for (int i = 0; i < nt; ++i) pts[count++] = {a.x + (b.x - a.x) * ts[i], a.y + (b.y - a.y) * ts[i]};
		pts[count++] = b;
		for (int i = 0; i + 1 < count; ++i) {
			Point p = pts[i], q = pts[i + 1];
			p.x = std::clamp(p.x, 0.0f, W);
			q.x = std::clamp(q.x, 0.0f, W);
			line(p, q, dirFlip_ ? -1.0f : 1.0f);
		}
	}

	// Coverage of row `row` (0-based within the rectangle), scaled by `alpha`
	// (0..1). Between the cells an edge touched the accumulator is empty and the
	// coverage holds, so the row is a sequence of constant runs, found from the
	// row's touched-cell bitmap without reading the empty cells at all. A run of
	// kSpanMin or more pixels goes to `span(x, count, coverage)` -- a fill's
	// interior is one call -- and the short runs at anti-aliased edges are
	// gathered into `out` and handed to `emit(x, coverage, count)`. Empty runs
	// emit nothing, so the hole of a ring or the gap between a wave's crossings
	// costs nothing downstream. Clears the row's accumulator cells and bitmap.
	static constexpr int kSpanMin = 8;
	template <typename Emit, typename Span>
	GEA_SVG_HOT void row(int row, std::uint8_t *out, bool evenOdd, float alpha, Emit &&emit, Span &&span)
	{
		const std::size_t r = static_cast<std::size_t>(row);
		std::uint32_t *bits = &bits_[r * static_cast<std::size_t>(wpr_)];
		int *a = &acc_[r * static_cast<std::size_t>(stride_)];
		const int end = w_;  // one past the last pixel
		const int alpha8 = static_cast<int>(alpha * 256.0f + 0.5f);  // 0..256
		int sum = 0;
		int runX = -1, runV = 0;
		int seg = -1, segEnd = 0;  // pending short runs, contiguous in `out`
		auto flush = [&](int from, int to, int v) {
			if (to > end) to = end;
			const int n = to - from;
			if (n <= 0) return;
			if (n >= kSpanMin) {
				if (seg >= 0) emit(seg, out + seg, segEnd - seg), seg = -1;
				if (v) span(from, n, static_cast<std::uint8_t>(v));
			} else {
				if (seg < 0) seg = from;
				for (int i = 0; i < n; ++i) out[from + i] = static_cast<std::uint8_t>(v);
				segEnd = to;
			}
		};
		for (int w = 0; w < wpr_; ++w) {
			std::uint32_t m = bits[w];
			if (!m) continue;
			bits[w] = 0;
			do {
				const int c = (w << 5) + __builtin_ctz(m);
				m &= m - 1;
				if (runX >= 0) flush(runX, c, runV);
				sum += a[c];
				a[c] = 0;
				int cov = sum < 0 ? -sum : sum;  // 65536 = full
				if (evenOdd) {
					cov &= 0x1ffff;
					if (cov > 0x10000) cov = 0x20000 - cov;
				} else if (cov > 0x10000) {
					cov = 0x10000;
				}
				// 65536 * 256 overflows nothing: cov <= 2^16, alpha8 <= 2^8.
				runV = static_cast<int>((static_cast<unsigned>(cov) * static_cast<unsigned>(alpha8) * 255u + (1u << 23)) >> 24);
				runX = c;
			} while (m);
		}
		// The last touched cell's own pixel; past it the row is empty.
		if (runX >= 0) flush(runX, runX + 1, runV);
		if (seg >= 0) emit(seg, out + seg, segEnd - seg);
	}

	template <typename Emit>
	void row(int row, std::uint8_t *out, bool evenOdd, float alpha, Emit &&emit)
	{
		this->row(row, out, evenOdd, alpha, emit, [&](int x, int n, std::uint8_t v) {
			std::memset(out + x, v, static_cast<std::size_t>(n));
			emit(x, out + x, n);
		});
	}

private:
	// Accumulates a top-to-bottom edge already inside [0,w] x [0,h].
	// Fixed-point accumulation. Coordinates are in 1/256 px and a fully
	// covered pixel accumulates 65536. Integer arithmetic is single-cycle on
	// the MCUs this runs on, where a dependent float multiply-add is ~13
	// cycles; and each row's contributions sum to exactly d * 256, so a closed
	// shape's winding always returns to zero (no streaks from rounding).
	static constexpr int kSub = 256;

	void line(Point p0, Point p1, float dir) { line(p0, p1, dir, (p1.x - p0.x) / (p1.y - p0.y)); }
	// `dxdy` is the piece's slope, passed in when the caller already has it.
	GEA_SVG_HOT void line(Point p0, Point p1, float dir, float dxdy)
	{
		const int y0s = static_cast<int>(p0.y * kSub + 0.5f);
		const int y1s = static_cast<int>(p1.y * kSub + 0.5f);
		if (y1s <= y0s) return;
		const int sign = dir < 0 ? -1 : 1;
		const int yStart = y0s >> 8;
		const int yEnd = std::min(h_, (y1s + kSub - 1) >> 8);
		const int maxX = w_ * kSub;
		// x in subpixels at subpixel row s: x0 + dxdy * (s - y0s). Each slice's
		// bottom is the next slice's top, so one evaluation per row.
		const float xBase = p0.x * kSub + 0.5f;
		auto xAt = [&](int s) {
			const int x = static_cast<int>(xBase + dxdy * static_cast<float>(s - y0s));
			return x < 0 ? 0 : (x > maxX ? maxX : x);
		};
		int xNext = xAt(std::max(y0s, yStart * kSub));
		for (int y = yStart; y < yEnd; ++y) {
			int *rowAcc = &acc_[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_)];
			const int top = std::max(y0s, y * kSub);
			const int bottom = std::min(y1s, (y + 1) * kSub);
			if (bottom <= top) continue;
			// x at this slice's top and bottom, in subpixels.
			const int xa = xNext;
			const int xb = xAt(bottom);
			xNext = xb;
			const int d = (bottom - top) * sign;  // vertical extent, 1/256 px
			const int total = d * kSub;           // this slice's whole contribution
			const int x0 = xa < xb ? xa : xb, x1 = xa < xb ? xb : xa;
			const int x0i = x0 >> 8;
			const int x1i = (x1 + kSub - 1) >> 8;
			markCells(&bits_[static_cast<std::size_t>(y) * static_cast<std::size_t>(wpr_)], x0i,
			          x0i + 1 > x1i ? x0i + 1 : x1i);
			if (x1i <= x0i + 1) {
				// Within one cell: split by the mean x inside it.
				const int xmf = ((xa + xb) >> 1) - (x0i << 8);
				const int right = d * xmf;
				rowAcc[x0i] += total - right;
				rowAcc[x0i + 1] += right;
			} else {
				const int wsub = x1 - x0;
				const int x0f = x0 - (x0i << 8);
				const int x1f = x1 - ((x1i - 1) << 8);  // (0, 256]
				// Divisions by wsub go through its reciprocal (2^24 / wsub): an
				// integer divide is a multi-cycle instruction on these cores and
				// a row needed up to four. A straight edge's full rows share one
				// or two widths (rounding), so the last two are kept. A quotient
				// may come out one low; the remainder below absorbs it, so every
				// row still sums to exactly `total`.
				if (wsub != recipW_[0]) {
					if (wsub == recipW_[1]) {
						std::swap(recipW_[0], recipW_[1]);
						std::swap(recip_[0], recip_[1]);
					} else {
						recipW_[1] = recipW_[0];
						recip_[1] = recip_[0];
						recipW_[0] = wsub;
						recip_[0] = (1u << 24) / static_cast<unsigned>(wsub);
					}
				}
				const std::uint64_t rcp = recip_[0];
				auto div = [rcp](unsigned num, int shift) { return static_cast<int>((num * rcp) >> shift); };
				// Fractions of `total` per cell, in 1/256 (see the float form:
				// a0 = s(1-x0f)^2/2, middle cells s, am = s x1f^2/2).
				const int a0 = div(static_cast<unsigned>((kSub - x0f) * (kSub - x0f)), 25);
				const int am = div(static_cast<unsigned>(x1f * x1f), 25);
				int assigned = d * a0;
				rowAcc[x0i] += assigned;
				if (x1i == x0i + 2) {
					const int mid = d * (kSub - a0 - am);
					rowAcc[x0i + 1] += mid;
					assigned += mid;
				} else {
					const int s = div(static_cast<unsigned>(kSub * kSub), 24);
					const int a1 = div(static_cast<unsigned>(kSub * (kSub + kSub / 2 - x0f)), 24);
					const int first = d * (a1 - a0);
					rowAcc[x0i + 1] += first;
					assigned += first;
					const int ds = d * s;
					for (int xi = x0i + 2; xi < x1i - 1; ++xi) {
						rowAcc[xi] += ds;
						assigned += ds;
					}
					const int a2 = a1 + (x1i - x0i - 3) * s;
					const int pen = d * (kSub - a2 - am);
					rowAcc[x1i - 1] += pen;
					assigned += pen;
				}
				rowAcc[x1i] += total - assigned;  // exact remainder
			}
		}
	}

	// Sets cells [lo, hi] in a row's touched-cell bitmap.
	static void markCells(std::uint32_t *bits, int lo, int hi)
	{
		const int wl = lo >> 5, wh = hi >> 5;
		const std::uint32_t first = ~0u << (lo & 31), last = ~0u >> (31 - (hi & 31));
		if (wl == wh) {
			bits[wl] |= first & last;
			return;
		}
		bits[wl] |= first;
		for (int w = wl + 1; w < wh; ++w) bits[w] = ~0u;
		bits[wh] |= last;
	}

	int x0_ = 0, y0_ = 0, w_ = 0, h_ = 0, stride_ = 2, wpr_ = 1;
	int recipW_[2] = {0, 0};
	unsigned recip_[2] = {0, 0};
	bool dirFlip_ = false;
	// fillCoverage's strip budget (2048 cells) plus the two guard columns of
	// its tallest strips; the bitmap's worst case is a narrow, tall strip.
	static constexpr std::size_t kReserveCells = 2048 + 64;
	static constexpr std::size_t kReserveWords = 256;
	int accRetry_ = 0, bitsRetry_ = 0;
	ScratchVector<int> acc_;
	ScratchVector<std::uint32_t> bits_;  // one bit per accumulator cell an edge touched
	ScratchVector<std::uint32_t> active_;
	std::size_t nextEdge_ = 0;
	EdgeList ownEdges_;
	const EdgeList *edges_ = nullptr;
};

// Rasterizes `edges` over the pixel rectangle [x0,x1) x [y0,y1) in strips
// sized to a fixed accumulator budget, calling `span(y, x, count, coverage)`
// for each long constant run and `emit(y, x, coverage, count)` for the short
// anti-aliased runs between them.
template <typename Emit, typename Span>
GEA_SVG_HOT inline void fillCoverage(Rasterizer &r, ScratchVector<std::uint8_t> &rowBuf, const EdgeList &edges,
                         int x0, int y0, int x1, int y1, bool evenOdd, float alpha, Emit &&emit, Span &&span)
{
	if (x1 <= x0 || y1 <= y0 || alpha <= 0 || edges.empty()) return;
	const int width = x1 - x0;
	constexpr int kBudget = 2048;  // accumulator cells per strip (8 KB, internal RAM)
	const int stripRows = std::max(1, kBudget / (width + 2));
	rowBuf.resize(static_cast<std::size_t>(width));
	r.useEdges(edges);
	r.beginEdgeWalk();
	for (int sy = y0; sy < y1; sy += stripRows) {
		const int rows = std::min(stripRows, y1 - sy);
		r.reset(x0, sy, width, rows);
		r.addActiveEdges();
		for (int row = 0; row < rows; ++row) {
			const int y = sy + row;
			r.row(
			    row, rowBuf.data(), evenOdd, alpha,
			    [&](int x, const std::uint8_t *cov, int count) { emit(y, x0 + x, cov, count); },
			    [&](int x, int count, std::uint8_t cov) { span(y, x0 + x, count, cov); });
		}
	}
}

// Same, with every run delivered as coverage bytes.
template <typename Emit>
inline void fillCoverage(Rasterizer &r, ScratchVector<std::uint8_t> &rowBuf, const EdgeList &edges,
                         int x0, int y0, int x1, int y1, bool evenOdd, float alpha, Emit &&emit)
{
	std::uint8_t *base = nullptr;
	fillCoverage(
	    r, rowBuf, edges, x0, y0, x1, y1, evenOdd, alpha,
	    [&](int y, int x, const std::uint8_t *cov, int count) { emit(y, x, cov, count); },
	    [&](int y, int x, int count, std::uint8_t cov) {
		    base = rowBuf.data() + (x - x0);
		    std::memset(base, cov, static_cast<std::size_t>(count));
		    emit(y, x, base, count);
	    });
}

template <typename Emit>
inline void fillCoverage(Rasterizer &r, ScratchVector<std::uint8_t> &rowBuf, const std::vector<Contour> &contours,
                         int x0, int y0, int x1, int y1, bool evenOdd, float alpha, Emit &&emit)
{
	EdgeList edges;
	buildEdges(contours, edges);
	fillCoverage(r, rowBuf, edges, x0, y0, x1, y1, evenOdd, alpha, emit);
}

}  // namespace gea::embedded::ui::svg
