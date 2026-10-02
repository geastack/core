// Native pipeline test for examples/apps/weather.
//
// The app boots with '--' placeholders, brings WiFi up from tick(), then
// fetches a live open-meteo forecast per pinned city. The test host stubs
// WifiBackend::connected() == true, and this test overrides the weak fetch
// hooks in packages/host/host/fetch.cpp to serve realistic per-city forecast
// bodies — exercising the full fetch -> typed JSON.parse -> store -> UI
// pipeline (chip temps, hero conditions, metrics, hourly/daily strips), plus
// city switching from cache, the C/F unit toggle, Refresh + toasts, hourly
// and daily horizontal scrolling, and pixel-level render checks.

#include "native_test_harness.h"

#include "app.h"
#include "graphics/font.h"
#include "host/backends.h"
#include "image.h"
#include "refresh_perf.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

// host/fetch.h references gea_cpp_value when GEA_CPP_VALUE_AVAILABLE is set,
// but this test TU has no gea_cpp_value definition — include it with the flag
// masked (same dance as packages/host/host/fetch.cpp).
#ifdef GEA_CPP_VALUE_AVAILABLE
#define GEA_WEATHER_TEST_RESTORE_CPP_VALUE_AVAILABLE 1
#undef GEA_CPP_VALUE_AVAILABLE
#endif
#include "host/fetch.h"
#ifdef GEA_WEATHER_TEST_RESTORE_CPP_VALUE_AVAILABLE
#define GEA_CPP_VALUE_AVAILABLE 1
#undef GEA_WEATHER_TEST_RESTORE_CPP_VALUE_AVAILABLE
#endif

#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>

extern void __gea_top_level();

namespace gea::framework::input {
bool InputBackend::consumeBackButton()
{
	return false;
}
}  // namespace gea::framework::input

// ---- canned open-meteo forecast server ------------------------------------
namespace {
std::vector<std::string> gFetchedUrls;

std::string forecastJson(double tempC, int weatherCode, double hiC, double loC, double feelsC, double windKph,
                         double precipMm, double humidPct)
{
	char buf[64];
	std::string out = "{\"current\":{\"time\":\"2026-07-06T09:00\"";
	auto num = [&](const char *key, double value) {
		std::snprintf(buf, sizeof(buf), ",\"%s\":%.1f", key, value);
		out += buf;
	};
	num("temperature_2m", tempC);
	num("apparent_temperature", feelsC);
	num("precipitation", precipMm);
	num("relative_humidity_2m", humidPct);
	num("wind_speed_10m", windKph);
	std::snprintf(buf, sizeof(buf), ",\"weather_code\":%d}", weatherCode);
	out += buf;

	out += ",\"hourly\":{\"time\":[";
	for (int i = 0; i < 16; ++i) {
		std::snprintf(buf, sizeof(buf), "%s\"2026-07-06T%02d:00\"", i ? "," : "", 9 + i > 23 ? 9 + i - 24 : 9 + i);
		out += buf;
	}
	out += "],\"temperature_2m\":[";
	for (int i = 0; i < 16; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", tempC + i * 0.5);
		out += buf;
	}
	out += "],\"weather_code\":[";
	for (int i = 0; i < 16; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%d", i ? "," : "", weatherCode);
		out += buf;
	}
	out += "]}";

	out += ",\"daily\":{\"time\":[";
	for (int i = 0; i < 14; ++i) {
		std::snprintf(buf, sizeof(buf), "%s\"2026-07-%02d\"", i ? "," : "", 6 + i);
		out += buf;
	}
	out += "],\"temperature_2m_max\":[";
	for (int i = 0; i < 14; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", hiC);
		out += buf;
	}
	out += "],\"temperature_2m_min\":[";
	for (int i = 0; i < 14; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", loC);
		out += buf;
	}
	out += "],\"weather_code\":[";
	for (int i = 0; i < 14; ++i) {
		std::snprintf(buf, sizeof(buf), "%s%d", i ? "," : "", weatherCode);
		out += buf;
	}
	out += "]}}";
	return out;
}

std::string forecastBodyForUrl(const std::string &url)
{
	// Coordinates from WeatherStore's default pinned cities. Values chosen so
	// the UI shows: chips 18°/13°/13°/30° (C) and 65°/56°/55°/86° (F).
	if (url.find("latitude=38.7223") != std::string::npos)  // Lisbon — Partly cloudy
		return forecastJson(18.2, 2, 24.3, 17.8, 17.2, 16.4, 2.2, 87.0);
	if (url.find("latitude=37.7749") != std::string::npos)  // San Francisco — Overcast
		return forecastJson(13.4, 3, 16.1, 11.2, 12.6, 22.0, 0.0, 78.0);
	if (url.find("latitude=52.52") != std::string::npos)  // Berlin — Rain
		return forecastJson(12.6, 61, 15.0, 9.4, 11.8, 14.0, 4.4, 90.0);
	if (url.find("latitude=35.6762") != std::string::npos)  // Tokyo — Clear
		return forecastJson(30.2, 0, 33.5, 26.0, 32.4, 9.0, 0.0, 65.0);
	return std::string();
}
}  // namespace

// Strong definitions override the weak test hooks in host/fetch.cpp.
namespace gea::framework::host {
void test_record_request(const std::string &url, const gea::host::FetchRequestInit & /*init*/)
{
	gFetchedUrls.push_back(url);
}

gea::host::FetchResponse test_canned_response(const std::string &url)
{
	gea::host::FetchResponse response;
	const std::string body = forecastBodyForUrl(url);
	if (body.empty()) {
		response.ok = false;
		response.status = 404;
		response.status_text = "Not Found";
		return response;
	}
	response.ok = true;
	response.status = 200;
	response.status_text = "OK";
	response.headers["content-type"] = "application/json";
	response.body.assign(body.begin(), body.end());
	return response;
}
}  // namespace gea::framework::host

namespace {
constexpr int kViewportWidth = 410;
constexpr int kViewportHeight = 502;
constexpr double kDevicePixelRatio = 1.5;

int maxLumaInBox(int x, int y, int width, int height)
{
	int maxLuma = 0;
	const int x1 = x + width;
	const int y1 = y + height;
	for (int py = y; py < y1; ++py) {
		for (int px = x; px < x1; ++px) {
			if (px < 0 || py < 0 || px >= kViewportWidth || py >= kViewportHeight) continue;
			const std::uint16_t pixel = gea::embedded::test::displayPixelAt(px, py);
			const int r = ((pixel >> 11) & 0x1f) * 255 / 31;
			const int g = ((pixel >> 5) & 0x3f) * 255 / 63;
			const int b = (pixel & 0x1f) * 255 / 31;
			const int luma = (r * 299 + g * 587 + b * 114) / 1000;
			if (luma > maxLuma) maxLuma = luma;
		}
	}
	return maxLuma;
}

int firstBrightPixelYInBox(int x, int y, int width, int height, int minLuma)
{
	const int x1 = x + width;
	const int y1 = y + height;
	for (int py = y; py < y1; ++py) {
		for (int px = x; px < x1; ++px) {
			if (px < 0 || py < 0 || px >= kViewportWidth || py >= kViewportHeight) continue;
			const std::uint16_t pixel = gea::embedded::test::displayPixelAt(px, py);
			const int r = ((pixel >> 11) & 0x1f) * 255 / 31;
			const int g = ((pixel >> 5) & 0x3f) * 255 / 63;
			const int b = (pixel & 0x1f) * 255 / 31;
			const int luma = (r * 299 + g * 587 + b * 114) / 1000;
			if (luma >= minLuma) return py;
		}
	}
	return -1;
}

int lastBrightPixelYInBox(int x, int y, int width, int height, int minLuma)
{
	const int x1 = x + width;
	const int y1 = y + height;
	for (int py = y1 - 1; py >= y; --py) {
		for (int px = x; px < x1; ++px) {
			if (px < 0 || py < 0 || px >= kViewportWidth || py >= kViewportHeight) continue;
			const std::uint16_t pixel = gea::embedded::test::displayPixelAt(px, py);
			const int r = ((pixel >> 11) & 0x1f) * 255 / 31;
			const int g = ((pixel >> 5) & 0x3f) * 255 / 63;
			const int b = (pixel & 0x1f) * 255 / 31;
			const int luma = (r * 299 + g * 587 + b * 114) / 1000;
			if (luma >= minLuma) return py;
		}
	}
	return -1;
}

int firstLaidOutTextNode(const std::string &text)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId : gea::embedded::test::nodesWithText(text, true)) {
		const auto &layout = tree.node(nodeId).layout;
		if (layout.width > 0 && layout.height > 0) return nodeId;
	}
	return -1;
}

void renderedNodeBox(int nodeId, int *x, int *y, int *width, int *height)
{
	const auto &node = gea::embedded::ui::Tree::instance().node(nodeId);
	int x0 = node.layout.x;
	int y0 = node.layout.y;
	int x1 = node.layout.x + node.layout.width - 1;
	int y1 = node.layout.y + node.layout.height - 1;
	gea::embedded::ui::ViewRenderer::transformedBounds(node, false, &x0, &y0, &x1, &y1);
	*x = x0;
	*y = y0;
	*width = x1 >= x0 ? x1 - x0 + 1 : 0;
	*height = y1 >= y0 ? y1 - y0 + 1 : 0;
}

bool hasPseudoChild(int nodeId)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int child = tree.node(nodeId).first_child; child >= 0; child = tree.node(child).next_sibling) {
		const std::string tag = gea::embedded::ui::tagFromId(tree.node(child).tag_id);
		if (tag == "::before" || tag == "::after") return true;
	}
	return false;
}

int cityChipWithText(const std::string &text)
{
	for (int chipId : gea::embedded::test::nodesWithClass("city-chip")) {
		if (gea::embedded::test::textContent(chipId).find(text) != std::string::npos) return chipId;
	}
	return -1;
}

int activeCityChipCount()
{
	int count = 0;
	for (int chipId : gea::embedded::test::nodesWithClass("city-chip")) {
		if (gea::embedded::ui::Tree::instance().hasClass(chipId, "is-active")) ++count;
	}
	return count;
}

bool expectCityChips(const std::vector<std::string> &expectedNames, const std::vector<std::string> &expectedTemps,
                     const char *phase)
{
	using namespace gea::embedded::test;
	const auto names = nodesWithClass("city-chip-name");
	const auto temps = nodesWithClass("city-chip-temp");
	if (names.size() != expectedNames.size() || temps.size() != expectedTemps.size()) {
		std::fprintf(stderr, "[test_gea_weather_main] %s: expected %zu visible city chips, got names=%zu temps=%zu\n",
		             phase, expectedNames.size(), names.size(), temps.size());
		return false;
	}
	for (std::size_t i = 0; i < expectedNames.size(); ++i) {
		if (textContent(names[i]) != expectedNames[i]) {
			std::fprintf(stderr, "[test_gea_weather_main] %s: expected chip %zu name '%s', got '%s'\n", phase, i,
			             expectedNames[i].c_str(), textContent(names[i]).c_str());
			return false;
		}
		if (textContent(temps[i]) != expectedTemps[i]) {
			std::fprintf(stderr, "[test_gea_weather_main] %s: expected chip %zu temp '%s', got '%s'\n", phase, i,
			             expectedTemps[i].c_str(), textContent(temps[i]).c_str());
			return false;
		}
	}
	return true;
}

int gClockMs = 16;

void pumpFrames(int frames)
{
	for (int i = 0; i < frames; ++i) {
		gClockMs += 16;
		gea::embedded::test::pumpFrame(gClockMs);
	}
	gea::embedded::test::refresh();
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using gea::embedded::ui::Tree;

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::embedded::ui::Document::setPreferredMountSize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	pumpFrame(16);
	refresh();
	if (std::getenv("GEA_DUMP_WEATHER_TREE")) dumpTree("test_gea_weather_main");

	// ---- boot: placeholder state before any forecast fetch resolves ----
	const std::string bootText = rootTextContent();
	for (const char *label : {"Cities", "Refresh", "Lisbon", "San Francisco", "Berlin", "Tokyo", "Next hours",
	                          "Hours", "Days", "Feels", "Wind", "Rain", "Humid"}) {
		if (!expectContains(bootText, label, "boot text", "test_gea_weather_main")) {
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	if (!expectCityChips({"Lisbon", "San Francisco", "Berlin", "Tokyo"}, {"--", "--", "--", "--"}, "boot")) {
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!gFetchedUrls.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected no fetch before WiFi bring-up tick, got %zu\n",
		             gFetchedUrls.size());
		return 1;
	}

	// ---- live data: tick() fetches every pinned city, active city first ----
	pumpFrames(40);
	if (gFetchedUrls.size() != 4) {
		std::fprintf(stderr, "[test_gea_weather_main] expected 4 forecast fetches, got %zu\n", gFetchedUrls.size());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const char *expectedLatOrder[] = {"latitude=38.7223", "latitude=37.7749", "latitude=52.52", "latitude=35.6762"};
	for (int i = 0; i < 4; ++i) {
		if (gFetchedUrls[i].find("https://api.open-meteo.com/v1/forecast?") != 0 ||
		    gFetchedUrls[i].find(expectedLatOrder[i]) == std::string::npos) {
			std::fprintf(stderr, "[test_gea_weather_main] expected fetch %d for %s, got %s\n", i, expectedLatOrder[i],
			             gFetchedUrls[i].c_str());
			return 1;
		}
	}
	if (!expectCityChips({"Lisbon", "San Francisco", "Berlin", "Tokyo"}, {"18°", "13°", "13°", "30°"}, "live")) {
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto liveChipNames = nodesWithClass("city-chip-name");
	for (std::size_t i : {std::size_t{2}, std::size_t{3}}) {
		const auto &nameNode = Tree::instance().node(liveChipNames[i]);
		const int naturalWidth = gea::embedded::ui::TextRenderer::measureWidth(
		    nameNode.text.c_str(), nameNode.style.font_id, nameNode.style.font_size);
		if (nameNode.layout.width != naturalWidth) {
			std::fprintf(stderr,
			             "[test_gea_weather_main] short city '%s' was flex-shrunk after its temperature grew, width=%d natural=%d\n",
			             nameNode.text.c_str(), nameNode.layout.width, naturalWidth);
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	const std::string liveText = rootTextContent();
	for (const char *label : {"18°", "Partly cloudy", "H 24° L 18°", "17°", "16 km/h", "0.09 in", "87%",
	                          "Lisbon District, PT", "9 AM", "8 PM"}) {
		if (!expectContains(liveText, label, "live text", "test_gea_weather_main")) {
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}

	// ---- chip typography + degree glyph atlas ----
	const auto cityChipNames = nodesWithClass("city-chip-name");
	const auto cityChipTemps = nodesWithClass("city-chip-temp");
	const auto &nameStyle = Tree::instance().node(cityChipNames[0]).style;
	const auto &tempStyle = Tree::instance().node(cityChipTemps[0]).style;
	if (nameStyle.font_size != 24 || tempStyle.font_size != 24) {
		std::fprintf(stderr, "[test_gea_weather_main] expected 16px chip fonts at dpr 1.5 (24 device px), got name=%d temp=%d\n",
		             nameStyle.font_size, tempStyle.font_size);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	gea::framework::graphics::Glyph degreeGlyph{};
	const auto font = gea::framework::graphics::FontRegistry::rasterizedFamily(tempStyle.font_id, tempStyle.font_size);
	if (!font.valid() || !font.glyph(0x00b0, &degreeGlyph) || degreeGlyph.codepoint != 0x00b0) {
		std::fprintf(stderr, "[test_gea_weather_main] expected generated weather font atlas to include U+00B0 degree sign\n");
		return 1;
	}
	const int degreeWidth = gea::embedded::ui::TextRenderer::measureWidth("18°", tempStyle.font_id, tempStyle.font_size);
	const int fallbackWidth = gea::embedded::ui::TextRenderer::measureWidth("18??", tempStyle.font_id, tempStyle.font_size);
	if (degreeWidth <= 0 || degreeWidth >= fallbackWidth) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected UTF-8 degree text width to be narrower than two fallback glyphs, got degree=%d fallback=%d\n",
		             degreeWidth, fallbackWidth);
		return 1;
	}

	// ---- city rail: horizontal overflow drag ----
	const auto cityRails = nodesWithClass("city-rail");
	if (cityRails.size() != 1) {
		std::fprintf(stderr, "[test_gea_weather_main] expected one city rail, got %zu\n", cityRails.size());
		return 1;
	}
	const int cityRailId = cityRails[0];
	const auto &cityRail = Tree::instance().node(cityRailId);
	if (cityRail.style.overflow_x != 2 || cityRail.layout.scroll_content_width <= cityRail.layout.width) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected horizontally overflowing city rail, overflowX=%d contentWidth=%d width=%d\n",
		             cityRail.style.overflow_x, cityRail.layout.scroll_content_width, cityRail.layout.width);
		return 1;
	}
	const int cityRailDragX = cityRail.layout.x + cityRail.layout.width / 2;
	const int cityRailDragY = cityRail.layout.y + cityRail.layout.height / 2;
	std::vector<std::uint16_t> cityRailPixelsBefore;
	cityRailPixelsBefore.reserve(static_cast<std::size_t>(cityRail.layout.width * cityRail.layout.height));
	for (int y = cityRail.layout.y; y < cityRail.layout.y + cityRail.layout.height; ++y) {
		for (int x = cityRail.layout.x; x < cityRail.layout.x + cityRail.layout.width; ++x) {
			cityRailPixelsBefore.push_back(gea::embedded::test::displayPixelAt(x, y));
		}
	}
	gClockMs += 100;
	setNativeNowMs(gClockMs);
	Tree::instance().pointerDown(cityRailDragX, cityRailDragY);
	gClockMs += 16;
	setNativeNowMs(gClockMs);
	if (!Tree::instance().pointerMove(cityRailDragX - 20, cityRailDragY + 40)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected horizontal city-rail pointer move to be consumed\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	Tree::instance().pointerUp();
	gClockMs += 16;
	gea::embedded::ui::refreshPerfStatsReset();
	pumpFrame(gClockMs);
	const auto cityRailScrollPerf = gea::embedded::ui::refreshPerfStatsRead();
	if (cityRailScrollPerf.rootScrollAccepted != 1 || cityRailScrollPerf.rootScrollRejected != 0) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] city rail drag missed scroll-only refresh, accepted=%d rejected=%d\n",
		             cityRailScrollPerf.rootScrollAccepted, cityRailScrollPerf.rootScrollRejected);
		return 1;
	}
	if (Tree::instance().node(cityRailId).layout.scroll_x <= 0 ||
	    Tree::instance().node(cityRailId).layout.scroll_y != 0) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected city rail to scroll only on x, scroll=(%d,%d) contentWidth=%d width=%d\n",
		             Tree::instance().node(cityRailId).layout.scroll_x,
		             Tree::instance().node(cityRailId).layout.scroll_y,
		             Tree::instance().node(cityRailId).layout.scroll_content_width,
		             Tree::instance().node(cityRailId).layout.width);
		return 1;
	}
	std::size_t changedCityRailPixels = 0;
	std::size_t cityRailPixelIndex = 0;
	for (int y = cityRail.layout.y; y < cityRail.layout.y + cityRail.layout.height; ++y) {
		for (int x = cityRail.layout.x; x < cityRail.layout.x + cityRail.layout.width; ++x, ++cityRailPixelIndex) {
			if (gea::embedded::test::displayPixelAt(x, y) != cityRailPixelsBefore[cityRailPixelIndex]) {
				++changedCityRailPixels;
			}
		}
	}
	if (changedCityRailPixels < 100) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] city rail scroll offset changed without presenting shifted pixels, changed=%zu\n",
		             changedCityRailPixels);
		return 1;
	}

	// ---- hero: temp value + CSS-absolute degree superscript ----
	const auto heroTemps = nodesWithClass("temp");
	const auto heroDegrees = nodesWithClass("degree");
	const auto tempRows = nodesWithClass("temp-row");
	if (heroTemps.empty() || heroDegrees.empty() || tempRows.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected hero temp/degree/temp-row nodes, got %zu/%zu/%zu\n",
		             heroTemps.size(), heroDegrees.size(), tempRows.size());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (textContent(heroTemps[0]) != "18") {
		std::fprintf(stderr, "[test_gea_weather_main] expected hero temp '18', got '%s'\n", textContent(heroTemps[0]).c_str());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto &tempRowLayout = Tree::instance().node(tempRows[0]).layout;
	const auto &heroDegreeNode = Tree::instance().node(heroDegrees[0]);
	// .degree is CSS position:absolute left:50px top:1px inside .temp-row → 75/±2 device px.
	const int degreeLeft = heroDegreeNode.layout.x - tempRowLayout.x;
	const int degreeTop = heroDegreeNode.layout.y - tempRowLayout.y;
	if (textContent(heroDegrees[0]) != "°" || heroDegreeNode.style.font_size != 24 || degreeLeft < 73 ||
	    degreeLeft > 77 || degreeTop < 0 || degreeTop > 6) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected hero degree as 16px absolute superscript, got text='%s' font=%d left=%d top=%d\n",
		             textContent(heroDegrees[0]).c_str(), heroDegreeNode.style.font_size, degreeLeft, degreeTop);
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- active-city highlight moves with chip clicks (cache, no refetch) ----
	if (activeCityChipCount() != 1 || !Tree::instance().hasClass(cityChipWithText("Lisbon"), "is-active")) {
		std::fprintf(stderr, "[test_gea_weather_main] expected exactly one active chip (Lisbon) initially, got %d\n",
		             activeCityChipCount());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!clickFirstText("San Francisco", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected San Francisco city chip to be clickable\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(2);
	const int lisbonChipId = cityChipWithText("Lisbon");
	const int sanFranciscoChipId = cityChipWithText("San Francisco");
	if (lisbonChipId < 0 || sanFranciscoChipId < 0 || activeCityChipCount() != 1 ||
	    Tree::instance().hasClass(lisbonChipId, "is-active") ||
	    !Tree::instance().hasClass(sanFranciscoChipId, "is-active")) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected active highlight to move to San Francisco only, total=%d lisbonActive=%d sanFranciscoActive=%d\n",
		             activeCityChipCount(), Tree::instance().hasClass(lisbonChipId, "is-active") ? 1 : 0,
		             Tree::instance().hasClass(sanFranciscoChipId, "is-active") ? 1 : 0);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	// Cached city switch renders instantly from the stored forecast: hero flips
	// to San Francisco's data without a new network fetch.
	const std::string sfText = rootTextContent();
	for (const char *label : {"San Francisco", "California, US", "Overcast", "H 16° L 11°"}) {
		if (!expectContains(sfText, label, "san francisco text", "test_gea_weather_main")) {
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	if (textContent(heroTemps[0]) != "13" || gFetchedUrls.size() != 4) {
		std::fprintf(stderr, "[test_gea_weather_main] expected cached SF switch (temp '13', still 4 fetches), got temp='%s' fetches=%zu\n",
		             textContent(heroTemps[0]).c_str(), gFetchedUrls.size());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!clickFirstText("Lisbon", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Lisbon city chip to be clickable after San Francisco\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(2);

	// Opening and disposing the conditional city-manager branch must remove its
	// add-button listener. The old body-delegated listener captured a node id that
	// was later recycled by WeatherView, so a city-chip click could spuriously call
	// addSearchResult() and show "Type at least two letters".
	if (!clickFirstText("Cities", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Cities button to open manager\n");
		return 1;
	}
	pumpFrames(2);
	if (!clickFirstText("Done", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Done button to close manager\n");
		return 1;
	}
	pumpFrames(2);
	if (!clickFirstText("San Francisco", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected SF chip after manager disposal\n");
		return 1;
	}
	pumpFrames(2);
	if (rootTextContent().find("Type at least two letters") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_weather_main] disposed city-manager add handler fired from a city chip\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!clickFirstText("Lisbon", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Lisbon chip after manager disposal\n");
		return 1;
	}
	pumpFrames(2);

	// ---- °C/°F toggle recomputes every temperature from cached data ----
	if (!clickFirstText("C", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected unit toggle 'C' to be clickable\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(2);
	if (!expectCityChips({"Lisbon", "San Francisco", "Berlin", "Tokyo"}, {"65°", "56°", "55°", "86°"}, "fahrenheit")) {
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const std::string fahrenheitText = rootTextContent();
	for (const char *label : {"F", "H 76° L 64°", "63°", "10 mph"}) {
		if (!expectContains(fahrenheitText, label, "fahrenheit text", "test_gea_weather_main")) {
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	if (textContent(heroTemps[0]) != "65") {
		std::fprintf(stderr, "[test_gea_weather_main] expected hero temp '65' in F, got '%s'\n",
		             textContent(heroTemps[0]).c_str());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!clickFirstText("F", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected unit toggle 'F' to be clickable\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(2);
	if (!expectCityChips({"Lisbon", "San Francisco", "Berlin", "Tokyo"}, {"18°", "13°", "13°", "30°"}, "back to C")) {
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- rendered text luma + CSS line-height fidelity ----
	for (const char *label : {"Lisbon", "Partly cloudy"}) {
		const int nodeId = firstLaidOutTextNode(label);
		if (nodeId < 0) {
			std::fprintf(stderr, "[test_gea_weather_main] missing exact text node %s\n", label);
			dumpTree("test_gea_weather_main");
			return 1;
		}
		const auto &layout = Tree::instance().node(nodeId).layout;
		if (layout.width <= 0 || layout.height <= 0) {
			std::fprintf(stderr, "[test_gea_weather_main] text node %s has empty layout (%d,%d %dx%d)\n", label,
			             layout.x, layout.y, layout.width, layout.height);
			dumpTree("test_gea_weather_main");
			return 1;
		}
		int renderX = layout.x;
		int renderY = layout.y;
		int renderWidth = layout.width;
		int renderHeight = layout.height;
		renderedNodeBox(nodeId, &renderX, &renderY, &renderWidth, &renderHeight);
		const int maxLuma = maxLumaInBox(renderX, renderY, renderWidth, renderHeight);
		if (maxLuma < 200) {
			std::fprintf(stderr, "[test_gea_weather_main] text node %s rendered too dark in (%d,%d %dx%d), max luma=%d\n",
			             label, renderX, renderY, renderWidth, renderHeight, maxLuma);
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}

	const auto placeNames = nodesWithClass("place-name");
	if (placeNames.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected place-name node\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	// place-name: 33px * 0.96 line-height * 1.5 dpr = 48; temp: 54px * 0.75 * 1.5 = 61.
	if (Tree::instance().node(placeNames[0]).style.line_height != 48 ||
	    Tree::instance().node(heroTemps[0]).style.line_height != 61) {
		std::fprintf(stderr, "[test_gea_weather_main] expected CSS line-height at dpr %.1f (place=48 temp=61), got place=%d temp=%d\n",
		             kDevicePixelRatio, Tree::instance().node(placeNames[0]).style.line_height,
		             Tree::instance().node(heroTemps[0]).style.line_height);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const int locationDetailNode = firstLaidOutTextNode("Lisbon District, PT");
	const int placeBottom = Tree::instance().node(placeNames[0]).layout.y + Tree::instance().node(placeNames[0]).layout.height;
	const int detailTop = locationDetailNode >= 0 ? Tree::instance().node(locationDetailNode).layout.y : -1;
	if (locationDetailNode < 0 || detailTop < placeBottom || detailTop - placeBottom > 6) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected location detail just below place name, detail=%d placeY=%d placeH=%d detailY=%d\n",
		             locationDetailNode, Tree::instance().node(placeNames[0]).layout.y,
		             Tree::instance().node(placeNames[0]).layout.height, detailTop);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	// Glyphs must land inside their CSS line boxes (place-name box tops at
	// y=127, temp box at y=217 in the current layout).
	const int renderedPlaceTop = firstBrightPixelYInBox(26, 118, 150, 70, 210);
	const int renderedTempTop = firstBrightPixelYInBox(26, 205, 70, 80, 210);
	if (renderedPlaceTop < 0 || renderedPlaceTop > 155 || renderedTempTop < 0 || renderedTempTop > 250) {
		std::fprintf(stderr, "[test_gea_weather_main] expected hero glyphs to honor CSS line boxes, placeTop=%d tempTop=%d\n",
		             renderedPlaceTop, renderedTempTop);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const gea::embedded::ui::DisplayCommand *tempCommand =
	    gea::embedded::ui::DisplayList::instance().nodeCommandAt(heroTemps[0], 0);
	const int renderedTempBottom = lastBrightPixelYInBox(26, 205, 70, 80, 210);
	if (!tempCommand || renderedTempBottom < 0 || renderedTempBottom > tempCommand->by + tempCommand->bh - 1) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] temp command must cover compressed-line-height glyph bottom, renderedBottom=%d commandY=%d commandH=%d\n",
		             renderedTempBottom, tempCommand ? tempCommand->by : -1, tempCommand ? tempCommand->bh : -1);
		return 1;
	}

	// ---- pre-rendered weather background: exactly one full-screen image ----
	const auto weatherShellNodes = nodesWithClass("weather-shell");
	if (weatherShellNodes.size() != 1) {
		std::fprintf(stderr, "[test_gea_weather_main] expected one weather-shell node, got %zu\n", weatherShellNodes.size());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto weatherBackgroundImages = nodesWithClass("weather-bg-img");
	int visibleWeatherBackgrounds = 0;
	for (int imageId : weatherBackgroundImages) {
		const auto &imageNode = Tree::instance().node(imageId);
		if (Tree::instance().hasClass(imageId, "is-hidden")) continue;
		if (imageNode.style.image_fit != 1 || imageNode.layout.width != kViewportWidth || imageNode.layout.height != kViewportHeight) {
			std::fprintf(stderr,
			             "[test_gea_weather_main] expected contained full-screen weather background, fit=%d layout=%dx%d\n",
			             imageNode.style.image_fit, imageNode.layout.width, imageNode.layout.height);
			return 1;
		}
		if (gea::framework::graphics::ImageStore::instance().currentAlpha(imageNode.image_id) != nullptr) {
			std::fprintf(stderr,
			             "[test_gea_weather_main] opaque RGB weather background retained an alpha plane\n");
			return 1;
		}
		++visibleWeatherBackgrounds;
	}
	if (visibleWeatherBackgrounds != 1) {
		std::fprintf(stderr, "[test_gea_weather_main] expected exactly one visible weather background, got %d\n",
		             visibleWeatherBackgrounds);
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- hero weather visual: exactly one visible condition image ----
	const auto weatherImages = nodesWithClass("weather-img");
	bool hasVisibleWeatherImage = false;
	for (int imageId : weatherImages) {
		if (Tree::instance().hasClass(imageId, "is-hidden")) continue;
		const auto &layout = Tree::instance().node(imageId).layout;
		int renderX = layout.x;
		int renderY = layout.y;
		int renderWidth = layout.width;
		int renderHeight = layout.height;
		renderedNodeBox(imageId, &renderX, &renderY, &renderWidth, &renderHeight);
		if (renderWidth > 0 && renderHeight > 0 && renderX + renderWidth > 120 && renderX < kViewportWidth &&
		    renderY + renderHeight > 120 && renderY < 360) {
			hasVisibleWeatherImage = true;
			break;
		}
	}
	if (!hasVisibleWeatherImage) {
		std::fprintf(stderr, "[test_gea_weather_main] expected a visible weather image in the hero area\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	for (int imageContainerId : nodesWithClass("weather-visual")) {
		if (hasPseudoChild(imageContainerId)) {
			std::fprintf(stderr, "[test_gea_weather_main] weather visual should not render pseudo glow nodes\n");
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	if (hasPseudoChild(weatherShellNodes[0])) {
		std::fprintf(stderr, "[test_gea_weather_main] weather shell should not render pseudo overlay nodes\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- hourly strip: icons, centered labels, horizontal-only scroll ----
	const auto initialForecastImages = nodesWithClass("forecast-icon-img");
	int visibleHourForecastImages = 0;
	bool containedHourForecastImage = false;
	for (int imageId : initialForecastImages) {
		const auto &imageNode = Tree::instance().node(imageId);
		const auto &layout = imageNode.layout;
		if (layout.width > 0 && layout.height > 0 && layout.y >= 360) {
			++visibleHourForecastImages;
			if (!containedHourForecastImage) {
				auto &images = gea::framework::graphics::ImageStore::instance();
				const int sourceWidth = images.width(imageNode.image_id);
				const int sourceHeight = images.height(imageNode.image_id);
				if (imageNode.style.image_fit != 1 || sourceWidth <= 0 || sourceWidth != sourceHeight || layout.width <= layout.height) {
					std::fprintf(stderr,
					             "[test_gea_weather_main] expected square forecast source with object-fit:contain in a wide box, fit=%d source=%dx%d layout=%dx%d\n",
					             imageNode.style.image_fit, sourceWidth, sourceHeight, layout.width, layout.height);
					return 1;
				}
				containedHourForecastImage = true;
			}
		}
	}
	if (visibleHourForecastImages < 6) {
		std::fprintf(stderr, "[test_gea_weather_main] expected visible forecast images in the hourly row, got %d\n",
		             visibleHourForecastImages);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto hourRows = nodesWithClass("hour-row");
	if (hourRows.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected horizontal forecast row\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto &hourRow = Tree::instance().node(hourRows[0]);
	if (hourRow.style.overflow != 2 || hourRow.style.overflow_x != 2 || hourRow.style.overflow_y == 2 ||
	    hourRow.layout.scroll_content_width <= hourRow.layout.width) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected hour row to use horizontal-only native overflow, overflow=%d overflowX=%d overflowY=%d contentWidth=%d width=%d\n",
		             static_cast<int>(hourRow.style.overflow), static_cast<int>(hourRow.style.overflow_x),
		             static_cast<int>(hourRow.style.overflow_y), hourRow.layout.scroll_content_width,
		             hourRow.layout.width);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	// CSS mask-image right fade: calc(100% - 15px) → 22.5 → 23 device px.
	if (hourRow.style.mask_right_fade_width != 23) {
		std::fprintf(stderr, "[test_gea_weather_main] expected hourly row to carry CSS right-edge mask fade, got %d\n",
		             hourRow.style.mask_right_fade_width);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto hourTracks = nodesWithClass("hour-track");
	if (!hourTracks.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] hour row should not use fake forecast track nodes\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto hours = nodesWithClass("hour");
	const auto forecastLabels = nodesWithClass("forecast-label");
	if (hours.empty() || forecastLabels.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected visible centered hourly cells\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto &firstHour = Tree::instance().node(hours[0]).layout;
	const auto &firstForecastLabel = Tree::instance().node(forecastLabels[0]).layout;
	const int firstLabelCenter = firstForecastLabel.x + firstForecastLabel.width / 2;
	const int firstHourCenter = firstHour.x + firstHour.width / 2;
	if (firstForecastLabel.width <= 0 || firstForecastLabel.width > firstHour.width ||
	    std::abs(firstLabelCenter - firstHourCenter) > 2) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected first hourly label centered inside its cell, label=(%d,%d %dx%d) hour=(%d,%d %dx%d)\n",
		             firstForecastLabel.x, firstForecastLabel.y, firstForecastLabel.width, firstForecastLabel.height,
		             firstHour.x, firstHour.y, firstHour.width, firstHour.height);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const int hourMaxScrollX = hourRow.layout.scroll_content_width - hourRow.layout.width;
	const int hourDragDelta = hourMaxScrollX > 20 ? 8 : 1;
	const int dragX = hourRow.layout.x + hourRow.layout.width / 2;
	const int dragY = hourRow.layout.y + hourRow.layout.height / 2;
	gClockMs += 100;
	setNativeNowMs(gClockMs);
	Tree::instance().pointerDown(dragX, dragY);
	gClockMs += 16;
	setNativeNowMs(gClockMs);
	if (!Tree::instance().pointerMove(dragX - hourDragDelta, dragY + 48)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected horizontal forecast pointer move to be consumed, rowW=%d scrollContentW=%d delta=%d drag=(%d,%d) overflowX=%d\n", hourRow.layout.width, hourRow.layout.scroll_content_width, hourDragDelta, dragX, dragY, (int)hourRow.style.overflow_x);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const int hourScrollAfterMove = Tree::instance().node(hourRows[0]).layout.scroll_x;
	const int hourVerticalScrollAfterMove = Tree::instance().node(hourRows[0]).layout.scroll_y;
	Tree::instance().pointerUp();
	const int hourScrollAfterUp = Tree::instance().node(hourRows[0]).layout.scroll_x;
	gClockMs += 16;
	pumpFrame(gClockMs);
	const int hourScrollAfterMomentum = Tree::instance().node(hourRows[0]).layout.scroll_x;
	const int hourVerticalScrollAfterMomentum = Tree::instance().node(hourRows[0]).layout.scroll_y;
	if (Tree::instance().node(hourRows[0]).layout.scroll_x <= 0) {
		const auto &hourRowAfterRefresh = Tree::instance().node(hourRows[0]);
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected native horizontal forecast drag to increase scroll_x, got %d after move=%d up=%d contentWidth=%d width=%d\n",
		             hourRowAfterRefresh.layout.scroll_x, hourScrollAfterMove, hourScrollAfterUp,
		             hourRowAfterRefresh.layout.scroll_content_width, hourRowAfterRefresh.layout.width);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (hourScrollAfterMomentum <= hourScrollAfterUp && hourMaxScrollX > hourScrollAfterUp) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected horizontal forecast momentum to continue scrolling, move=%d up=%d momentum=%d max=%d\n",
		             hourScrollAfterMove, hourScrollAfterUp, hourScrollAfterMomentum, hourMaxScrollX);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (hourVerticalScrollAfterMove != 0 || hourVerticalScrollAfterMomentum != 0) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected horizontal forecast row to ignore vertical drag, yAfterMove=%d yAfterMomentum=%d\n",
		             hourVerticalScrollAfterMove, hourVerticalScrollAfterMomentum);
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- daily strip via the Days tab ----
	if (!clickFirstText("Days", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Days tab to be clickable\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(2);
	const std::string daysText = rootTextContent();
	for (const char *label : {"Next days", "Monday", "Tuesday", "24° / 18°"}) {
		if (!expectContains(daysText, label, "days text", "test_gea_weather_main")) {
			dumpTree("test_gea_weather_main");
			return 1;
		}
	}
	const auto dayRows = nodesWithClass("day-row");
	if (dayRows.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected horizontal day row\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (Tree::instance().node(dayRows[0]).style.overflow != 2 ||
	    Tree::instance().node(dayRows[0]).style.overflow_x != 2 ||
	    Tree::instance().node(dayRows[0]).style.overflow_y == 2 ||
	    Tree::instance().node(dayRows[0]).layout.scroll_content_width <= Tree::instance().node(dayRows[0]).layout.width) {
		const auto &dayRowForError = Tree::instance().node(dayRows[0]);
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected day row to use horizontal-only native overflow, overflow=%d overflowX=%d overflowY=%d contentWidth=%d width=%d\n",
		             static_cast<int>(dayRowForError.style.overflow), static_cast<int>(dayRowForError.style.overflow_x),
		             static_cast<int>(dayRowForError.style.overflow_y), dayRowForError.layout.scroll_content_width,
		             dayRowForError.layout.width);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (Tree::instance().node(dayRows[0]).style.mask_right_fade_width != 23) {
		std::fprintf(stderr, "[test_gea_weather_main] expected day row to carry CSS right-edge mask fade, got %d\n",
		             Tree::instance().node(dayRows[0]).style.mask_right_fade_width);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto dayTracks = nodesWithClass("day-track");
	if (!dayTracks.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] day row should not use fake forecast track nodes\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto dayForecastImages = nodesWithClass("forecast-icon-img");
	int visibleDayForecastImages = 0;
	for (int imageId : dayForecastImages) {
		const auto &layout = Tree::instance().node(imageId).layout;
		if (layout.width > 0 && layout.height > 0 && layout.y >= 360) ++visibleDayForecastImages;
	}
	if (visibleDayForecastImages < 5) {
		std::fprintf(stderr, "[test_gea_weather_main] expected visible forecast images in the daily row, got %d\n",
		             visibleDayForecastImages);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const auto &dayRow = Tree::instance().node(dayRows[0]);
	const int dayDragX = dayRow.layout.x + dayRow.layout.width / 2;
	const int dayDragY = dayRow.layout.y + dayRow.layout.height / 2;
	gClockMs += 100;
	setNativeNowMs(gClockMs);
	Tree::instance().pointerDown(dayDragX, dayDragY);
	gClockMs += 16;
	setNativeNowMs(gClockMs);
	if (!Tree::instance().pointerMove(dayDragX - 80, dayDragY + 48)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected horizontal day pointer move to be consumed\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const int dayVerticalScrollAfterMove = Tree::instance().node(dayRows[0]).layout.scroll_y;
	Tree::instance().pointerUp();
	refresh();
	if (Tree::instance().node(dayRows[0]).layout.scroll_x <= 0) {
		std::fprintf(stderr, "[test_gea_weather_main] expected native horizontal day drag to increase scroll_x, got %d\n",
		             Tree::instance().node(dayRows[0]).layout.scroll_x);
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (dayVerticalScrollAfterMove != 0 || Tree::instance().node(dayRows[0]).layout.scroll_y != 0) {
		std::fprintf(stderr,
		             "[test_gea_weather_main] expected horizontal day row to ignore vertical drag, yAfterMove=%d yAfterRefresh=%d\n",
		             dayVerticalScrollAfterMove, Tree::instance().node(dayRows[0]).layout.scroll_y);
		dumpTree("test_gea_weather_main");
		return 1;
	}

	// ---- Refresh: placeholders + toast, then refetch of all pinned cities ----
	const std::size_t fetchesBeforeRefresh = gFetchedUrls.size();
	if (!clickFirstText("Refresh", true)) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Refresh button to be clickable\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(1);
	const auto toasts = nodesWithClass("toast");
	if (toasts.empty()) {
		std::fprintf(stderr, "[test_gea_weather_main] expected toast node\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const std::string refreshToast = textContent(toasts[0]);
	if (refreshToast.find("Updating forecast") == std::string::npos ||
	    !Tree::instance().hasClass(toasts[0], "is-visible")) {
		std::fprintf(stderr, "[test_gea_weather_main] expected visible 'Updating forecast' toast after Refresh, got '%s'\n",
		             refreshToast.c_str());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	pumpFrames(40);
	if (gFetchedUrls.size() != fetchesBeforeRefresh + 4) {
		std::fprintf(stderr, "[test_gea_weather_main] expected Refresh to refetch all 4 pinned cities, got %zu -> %zu\n",
		             fetchesBeforeRefresh, gFetchedUrls.size());
		dumpTree("test_gea_weather_main");
		return 1;
	}
	if (!expectCityChips({"Lisbon", "San Francisco", "Berlin", "Tokyo"}, {"18°", "13°", "13°", "30°"}, "after refresh")) {
		dumpTree("test_gea_weather_main");
		return 1;
	}
	const std::string refreshedToast = textContent(toasts[0]);
	if (refreshedToast.find("Forecast refreshed") == std::string::npos) {
		std::fprintf(stderr, "[test_gea_weather_main] expected 'Forecast refreshed' toast after refetch, got '%s'\n",
		             refreshedToast.c_str());
		dumpTree("test_gea_weather_main");
		return 1;
	}

	if (displayNonzeroPixelCount() <= 0) {
		std::fprintf(stderr, "[test_gea_weather_main] expected rendered pixels\n");
		dumpTree("test_gea_weather_main");
		return 1;
	}

	return 0;
}
