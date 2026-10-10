#include "native_test_harness.h"

class gea_cpp_value;

#include "audio.h"
#include "camera.h"
#include "canvas.h"
#include "css/engine.h"
#include "display.h"
#include "image.h"
#include "app.h"
#include "host/audio.h"
#include "host/apps.h"
#include "host/backends.h"
#include "host/display_orientation.h"
#include "host/image.h"
#include "host/media.h"
#pragma push_macro("GEA_CPP_VALUE_AVAILABLE")
#undef GEA_CPP_VALUE_AVAILABLE
#include "host/rtc.h"
#include "host/websocket.h"
#include "host/http.h"
#pragma pop_macro("GEA_CPP_VALUE_AVAILABLE")
#include "host/timers.h"
#include "memory.h"
#include "power.h"
#include "services/frame_scheduler.h"
#include "services/storage_service.h"
#include "touch.h"
#include "ui/document.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace gea::framework::app::generated {
void drainMicrotasks();
}

namespace {

gea::framework::graphics::Canvas gDisplayCanvas;
int gDisplayWidth = gea::platform::display::kWidth;
int gDisplayHeight = gea::platform::display::kHeight;
std::vector<std::uint16_t> gDisplayPixels(static_cast<std::size_t>(gDisplayWidth * gDisplayHeight));
std::vector<std::uint16_t> gPresentedPixels(static_cast<std::size_t>(gDisplayWidth * gDisplayHeight));
std::uint8_t gDisplayAlpha = 255;
int gFlushCalls = 0;
int gFlushRects = 0;
int gFlushPixels = 0;
int gImageLoads = 0;
std::vector<std::vector<std::uint16_t>> gTestImagePixels;
std::unordered_map<std::string, std::string> gStorage;
std::unordered_map<const void *, int> gAssetDataImageCache;
std::unordered_map<std::string, int> gAssetPathImageCache;
std::string gStorageKvBlob;
std::string gLastLaunchedApp;
int gFrameIntervalMs = gea::framework::services::FrameScheduler::kDefaultFrameIntervalMs;
int gNowMs = 0;
bool gTouching = false;
int gTouchX = 0;
int gTouchY = 0;

void resetDisplay()
{
	if (gDisplayWidth < 1) gDisplayWidth = 1;
	if (gDisplayHeight < 1) gDisplayHeight = 1;
	gDisplayPixels.resize(static_cast<std::size_t>(gDisplayWidth * gDisplayHeight));
	gPresentedPixels.resize(static_cast<std::size_t>(gDisplayWidth * gDisplayHeight));
	std::fill(gDisplayPixels.begin(), gDisplayPixels.end(), static_cast<std::uint16_t>(0));
	std::fill(gPresentedPixels.begin(), gPresentedPixels.end(), static_cast<std::uint16_t>(0));
	gDisplayCanvas.bindPixels(gDisplayPixels.data(), gDisplayWidth, gDisplayHeight);
	gDisplayCanvas.resetClip();
	gDisplayCanvas.setGlobalAlpha(255);
	gDisplayAlpha = 255;
	gFlushCalls = 0;
	gFlushRects = 0;
	gFlushPixels = 0;
}

bool isVisibleNode(int nodeId)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
		if (tree.node(current).computedStyle().display == 1) return false;
	}
	return true;
}

gea::framework::graphics::ImageStore &imageStore()
{
	return gea::framework::graphics::ImageStore::instance();
}

int integer(double value)
{
	return static_cast<int>(value);
}

int registerSyntheticImage()
{
	constexpr int kImageSize = 36;
	const std::uint16_t colors[] = {
		0xffff,
		0x07e0,
		0xf800,
		0x001f,
	};
	const std::uint16_t color = colors[static_cast<std::size_t>(gImageLoads) % (sizeof(colors) / sizeof(colors[0]))];

	gTestImagePixels.emplace_back(static_cast<std::size_t>(kImageSize * kImageSize));
	std::fill(gTestImagePixels.back().begin(), gTestImagePixels.back().end(), color);
	return imageStore().registerBuffer(gTestImagePixels.back().data(), kImageSize, kImageSize);
}

double loadSyntheticImage()
{
	++gImageLoads;
	return static_cast<double>(registerSyntheticImage());
}

void appendText(int nodeId, std::string &out)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
	if (!isVisibleNode(nodeId)) return;
	const auto &node = tree.node(nodeId);
	if (node.text[0] != '\0') out += node.text;
	for (int child = node.first_child; child >= 0; child = tree.node(child).next_sibling) appendText(child, out);
}

// Saved reference engines retain the original direct arrays.
#ifndef GEA_CSS_POSITION_PX
#define GEA_CSS_POSITION_PX(style, side) ((style).pos_offsets[side])
#endif

void dumpNode(int nodeId, int indent)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
	const auto &node = tree.node(nodeId);
	const std::string pad(static_cast<std::size_t>(indent), ' ');
	std::fprintf(stderr,
	             "%s#%d type=%d class=\"%s\" text=\"%s\" left=%d top=%d width=%d height=%d layout=(%d,%d %dx%d) opacity=%u display=%d events=%d\n",
	             pad.c_str(),
	             nodeId,
	             static_cast<int>(node.type),
	             tree.className(nodeId).c_str(),
	             node.text.c_str(),
	             GEA_CSS_POSITION_PX(node.computedStyle(), 3),
	             GEA_CSS_POSITION_PX(node.computedStyle(), 0),
	             node.computedStyle().width,
	             node.computedStyle().height,
	             node.layout.x,
	             node.layout.y,
	             node.layout.width,
	             node.layout.height,
	             static_cast<unsigned>(node.computedStyle().opacity),
	             static_cast<int>(node.computedStyle().display),
	             tree.hasEventListener(nodeId) ? 1 : 0);
	for (int child = node.first_child; child >= 0; child = tree.node(child).next_sibling) dumpNode(child, indent + 2);
}

}  // namespace

namespace gea::embedded::test {

void resetNativeHost()
{
	gDisplayWidth = gea::platform::display::kWidth;
	gDisplayHeight = gea::platform::display::kHeight;
	gea::host::resetAnimationFrameCallbacks();
	gea::host::resetScheduledTimers();
	gea::embedded::ui::Document::setPreferredMountSize(gea::platform::display::kWidth, gea::platform::display::kHeight);
	gea::embedded::ui::setViewportMetrics(gea::platform::display::kWidth, gea::platform::display::kHeight, 1.0);
	gea::platform::display::Display::setAA(0);
	gea::embedded::ui::Document::instance().clear();
	resetDisplay();
	imageStore().disposeAll();
	gTestImagePixels.clear();
	gStorage.clear();
	gAssetDataImageCache.clear();
	gAssetPathImageCache.clear();
	gStorageKvBlob.clear();
	gImageLoads = 0;
	gLastLaunchedApp.clear();
	gNowMs = 0;
	gTouching = false;
	gTouchX = 0;
	gTouchY = 0;
}

void setNativeDisplaySize(int width, int height)
{
	gDisplayWidth = width > 0 ? width : 1;
	gDisplayHeight = height > 0 ? height : 1;
	resetDisplay();
}

void refresh()
{
	gea::framework::app::generated::drainMicrotasks();
	gea::embedded::ui::Document::instance().refreshMountedIfDirty();
}

void pumpFrame(double timestampMs)
{
	gNowMs = static_cast<int>(timestampMs);
	// Mirror production's per-app _frame() ordering EXACTLY (see CMakeLists.txt
	// emit at @PREFIX@_frame): drain stale microtasks FIRST, then runAF, then
	// frame, then refresh. NOTE: microtasks queued during runAF are NOT drained
	// in the same frame — they live in the shared gea_cpp_microtask_queue() and
	// are drained at the start of the NEXT frame. Earlier versions of this test
	// helper drained twice per frame, which hid a real production bug: stale
	// reactive-binding microtasks from the previous app surviving past the app
	// switch and firing against the new app's freshly-mounted nodes.
	gea::framework::app::generated::drainMicrotasks();
	gea::framework::display::DisplayBackend::updateAutoRotationFromAccelerometer();
	gea::host::runAnimationFrameCallbacks(timestampMs);
	gea::host::websocket::runCallbacks();
	gea::host::rtc::runCallbacks();
	gea::host::http::runRequests();
	gea::css::AnimationEngine::instance().tick(static_cast<std::uint32_t>(gNowMs));
	gea::embedded::ui::Document::instance().frame(static_cast<int>(timestampMs));
	gea::embedded::ui::Document::instance().refreshMountedIfDirty();
}

void setNativeNowMs(int timestampMs)
{
	gNowMs = timestampMs < 0 ? 0 : timestampMs;
}

std::string textContent(int nodeId)
{
	std::string out;
	appendText(nodeId, out);
	return out;
}

std::string rootTextContent()
{
	return textContent(gea::embedded::ui::Tree::instance().mountedRoot());
}

std::vector<int> nodesWithClass(const std::string &className)
{
	std::vector<int> out;
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		if (isVisibleNode(nodeId) && tree.hasClass(nodeId, className)) out.push_back(nodeId);
	}
	return out;
}

std::vector<int> nodesWithText(const std::string &text, bool exact)
{
	std::vector<int> out;
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		if (!isVisibleNode(nodeId)) continue;
		const std::string value = tree.node(nodeId).text;
		const bool matches = exact ? value == text : value.find(text) != std::string::npos;
		if (matches) out.push_back(nodeId);
	}
	return out;
}

std::vector<int> nodesWithType(gea::embedded::ui::NodeType type)
{
	std::vector<int> out;
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		if (isVisibleNode(nodeId) && tree.node(nodeId).type == type) out.push_back(nodeId);
	}
	return out;
}

std::vector<int> nodesWithBox(int width, int height)
{
	std::vector<int> out;
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); ++nodeId) {
		if (!isVisibleNode(nodeId)) continue;
		const auto &node = tree.node(nodeId);
		if ((node.computedStyle().width == width || node.layout.width == width) &&
		    (node.computedStyle().height == height || node.layout.height == height)) {
			out.push_back(nodeId);
		}
	}
	return out;
}

int firstNodeWithText(const std::string &text, bool exact)
{
	const auto nodes = nodesWithText(text, exact);
	return nodes.empty() ? -1 : nodes.front();
}

std::size_t countDescendantsWithClass(int nodeId, const std::string &className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (nodeId < 0 || nodeId >= tree.nodeCount()) return 0;
	if (!isVisibleNode(nodeId)) return 0;
	std::size_t count = tree.hasClass(nodeId, className) ? 1 : 0;
	for (int child = tree.node(nodeId).first_child; child >= 0; child = tree.node(child).next_sibling) {
		count += countDescendantsWithClass(child, className);
	}
	return count;
}

// Simulates a tap on a node carrying its pressId/pressValue. Since press
// collapsed into click, this fires a Click event (apps' onClick handlers read
// event.pressId/pressValue, same as the keyboard/HID routing does on device).
bool dispatchPress(int targetId, int pressId, int pressValue)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (targetId < 0 || targetId >= tree.nodeCount()) return false;
	const auto &node = tree.node(targetId);
	gea::framework::events::PointerEvent event{gea::framework::events::PointerEventType::Click};
	event.targetId = targetId;
	event.x = node.layout.x + node.layout.width / 2;
	event.y = node.layout.y + node.layout.height / 2;
	event.clientX = event.x;
	event.clientY = event.y;
	event.pageX = event.x;
	event.pageY = event.y;
	event.screenX = event.x;
	event.screenY = event.y;
	event.pressId = pressId;
	event.pressValue = pressValue;
	return tree.dispatchEvent(event);
}

bool dispatchClick(int targetId)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (targetId < 0 || targetId >= tree.nodeCount()) return false;
	const auto &node = tree.node(targetId);
	gea::framework::events::PointerEvent event{gea::framework::events::PointerEventType::Click};
	event.targetId = targetId;
	event.x = node.layout.x + node.layout.width / 2;
	event.y = node.layout.y + node.layout.height / 2;
	event.clientX = event.x;
	event.clientY = event.y;
	event.pageX = event.x;
	event.pageY = event.y;
	event.screenX = event.x;
	event.screenY = event.y;
	return tree.dispatchEvent(event);
}

void dispatchTouch(gea::framework::events::TouchPhase phase, bool touching, int x, int y)
{
	gTouching = touching;
	gTouchX = x;
	gTouchY = y;
	gea::framework::events::Event event{};
	event.type = gea::framework::events::EventType::Touch;
	event.touchPhase = phase;
	event.touching = touching;
	event.x = x;
	event.y = y;
	gea::framework::events::TouchRuntime::dispatchEvent(event);
}

bool pressFirstText(const std::string &text, bool exact)
{
	return dispatchPress(firstNodeWithText(text, exact));
}

bool clickFirstText(const std::string &text, bool exact)
{
	return dispatchClick(firstNodeWithText(text, exact));
}

bool expectContains(const std::string &haystack, const char *needle, const char *label, const char *testName)
{
	if (haystack.find(needle) != std::string::npos) return true;
	std::fprintf(stderr, "[%s] expected %s to contain %s, got:\n%s\n", testName, label, needle, haystack.c_str());
	return false;
}

void dumpTree(const char *testName)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	std::fprintf(stderr, "[%s] nodeCount=%d mountedRoot=%d\n", testName, tree.nodeCount(), tree.mountedRoot());
	dumpNode(tree.mountedRoot(), 0);
}

std::string lastLaunchedApp()
{
	return gLastLaunchedApp;
}

void clearLastLaunchedApp()
{
	gLastLaunchedApp.clear();
}

int imageLoadCount()
{
	return gImageLoads;
}

int flushCallCount()
{
	return gFlushCalls;
}

int flushRectCount()
{
	return gFlushRects;
}

int flushPixelCount()
{
	return gFlushPixels;
}

int displayNonzeroPixelCount()
{
	return static_cast<int>(std::count_if(gDisplayPixels.begin(), gDisplayPixels.end(), [](std::uint16_t pixel) {
		return pixel != 0;
	}));
}

std::uint16_t displayPixelAt(int x, int y)
{
	if (x < 0 || y < 0 || x >= gDisplayWidth || y >= gDisplayHeight) return 0;
	return gDisplayPixels[static_cast<std::size_t>(y * gDisplayWidth + x)];
}

std::uint16_t presentedPixelAt(int x, int y)
{
	if (x < 0 || y < 0 || x >= gDisplayWidth || y >= gDisplayHeight) return 0;
	return gPresentedPixels[static_cast<std::size_t>(y * gDisplayWidth + x)];
}

	}  // namespace gea::embedded::test

#ifndef GEA_NATIVE_TEST_REAL_IMAGE_HOST
namespace gea::platform::storage {
void setMountProvider(bool (*)(void)) {}
bool ensureMounted() { return false; }
}  // namespace gea::platform::storage
#endif

namespace gea::platform::power {
int Power::batteryPercent() { return 100; }
bool Power::charging() { return false; }
}  // namespace gea::platform::power

// A test must never set the machine's clock, so the host refuses the request.
namespace gea::platform::clock {
bool setEpochMs(double) { return false; }
}  // namespace gea::platform::clock

// Null platform camera: the native test host has no capture hardware, so the
// host camera bridge (packages/host/host/camera.cpp) links against a camera
// that reports "not available" and fails every operation gracefully. Camera
// apps still mount and render; <camera> leaves show the built-in placeholder.
namespace gea::platform::camera {
bool Camera::isAvailable() { return false; }
bool Camera::hasPermission() { return false; }
bool Camera::requestPermission() { return false; }
bool Camera::open(const std::string &, int, int) { return false; }
void Camera::close() {}
bool Camera::isOpen() { return false; }
int Camera::width() { return 0; }
int Camera::height() { return 0; }
int Camera::orientation() { return 0; }
Facing Camera::currentFacing() { return Facing::Back; }
std::string Camera::currentFacingString() { return "back"; }
int Camera::deviceCount() { return 0; }
DeviceInfo Camera::deviceAt(int) { return {}; }
void Camera::drawPreview(int, int, int, int) {}
int Camera::previewMode() { return 0; }
bool Camera::fillPreview(gea::framework::graphics::pixel::native_t *, int, int, int, bool) { return false; }
void Camera::positionPreviewLayer(int, int, int, int) {}
void Camera::hidePreviewLayer() {}
void Camera::presentNativeOverlay() {}
int Camera::capture(bool) { return -1; }
bool Camera::startRecording(const std::string &, double) { return false; }
double Camera::stopRecording() { return -1.0; }
bool Camera::isRecording() { return false; }
void Camera::setFlash(const std::string &) {}
void Camera::setZoom(double) {}
void Camera::setMirror(bool) {}
void Camera::setExposure(const std::string &, double, double, double) {}
void Camera::setWhiteBalance(const std::string &, double, double) {}
void Camera::setFocus(const std::string &, double, double) {}
void Camera::setTorch(const std::string &, double) {}
}  // namespace gea::platform::camera

namespace gea::host {

namespace websocket {
void runCallbacks() {}
}  // namespace websocket

namespace rtc {
void runCallbacks() {}
}  // namespace rtc

AudioDestinationProperty::operator AudioDestinationNode() const { return AudioDestinationNode(1.0); }
AudioDestinationProperty::operator double() const { return 1.0; }
AudioContextCurrentTimeProperty::operator double() const { return 0.0; }
const AudioParamValueProperty &AudioParamValueProperty::operator=(double /*frequency_hz*/) const { return *this; }
AudioParamValueProperty::operator double() const { return 440.0; }
const AudioParam &AudioParam::operator=(double /*frequency_hz*/) const { return *this; }
void AudioParam::setValueAtTime(double /*frequency_hz*/, double /*start_time*/) const {}
const OscillatorTypeProperty &OscillatorTypeProperty::operator=(double /*type*/) const { return *this; }
const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const char * /*type*/) const { return *this; }
const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const std::string & /*type*/) const { return *this; }
void OscillatorNode::connect(AudioDestinationNode /*destination*/) const {}
void OscillatorNode::connect(AudioDestinationProperty /*destination*/) const {}
void OscillatorNode::connect(double /*destinationHandle*/) const {}
void OscillatorNode::start(double /*when*/) const {}
void OscillatorNode::stop(double /*when*/) const {}
// The null host has no audio clock or worklet state; the context only carries its rate.
AudioContext::AudioContext(double rate) : sampleRate(rate) {}
OscillatorNode AudioContext::createOscillator() const { return OscillatorNode(2.0); }

// Null HTMLAudioElement: the test host has no audio output; construction and
// control succeed silently.
struct HTMLAudioElement::State {
  std::string src;
  int nodeId = -1;
};
HTMLAudioElement HTMLAudioElement::create() {
	HTMLAudioElement result;
	result.state_ = std::make_shared<State>();
	return result;
}
HTMLAudioElement HTMLAudioElement::create(const std::string &src) { return HTMLAudioElement(src); }
HTMLAudioElement::HTMLAudioElement(const char *src) : HTMLAudioElement(std::string(src ? src : "")) {}
HTMLAudioElement::HTMLAudioElement(const std::string &src) : state_(std::make_shared<State>()) { state_->src = src; }
HTMLAudioElement::HTMLAudioElement(const gea::embedded::ui::NodeHandle &node) : state_(std::make_shared<State>()) { state_->nodeId = node.id(); }
std::string HTMLAudioElement::src() const { return state_ ? state_->src : std::string{}; }
void HTMLAudioElement::setSrc(const std::string &src) {
  if (!state_) state_ = std::make_shared<State>();
  state_->src = src;
}
bool HTMLAudioElement::play() const { return true; }
void HTMLAudioElement::pause() const {}

#ifndef GEA_NATIVE_TEST_REAL_IMAGE_HOST
double ImageService::loadBytes(std::vector<std::uint8_t> /*bytes*/) const { return loadSyntheticImage(); }
double ImageService::loadBytesOpaque(std::vector<std::uint8_t> /*bytes*/) const { return loadSyntheticImage(); }
double ImageService::loadBytesPtr(const std::uint8_t *data, std::size_t length) const
{
	if (!data || length == 0) return -1.0;
	const auto cached = gAssetDataImageCache.find(data);
	if (cached != gAssetDataImageCache.end()) return static_cast<double>(cached->second);
	const int imageId = static_cast<int>(loadSyntheticImage());
	gAssetDataImageCache.emplace(data, imageId);
	return static_cast<double>(imageId);
}
double ImageService::loadAssetPath(const char *path) const
{
	if (!path || path[0] == '\0') return -1.0;
	const auto cached = gAssetPathImageCache.find(path);
	if (cached != gAssetPathImageCache.end()) return static_cast<double>(cached->second);
	const int imageId = static_cast<int>(loadSyntheticImage());
	gAssetPathImageCache.emplace(path, imageId);
	return static_cast<double>(imageId);
}
double ImageService::loadFile(const std::string & /*path*/) const { return -1.0; }
double ImageService::loadFileOpaque(const std::string & /*path*/) const { return -1.0; }
bool ImageService::writeFile(const std::string & /*path*/, const std::vector<std::uint8_t> & /*bytes*/) const { return false; }
std::vector<std::uint8_t> ImageService::readFile(const std::string & /*path*/) const { return {}; }
std::vector<std::uint8_t> ImageService::readMapArchive() const { return {}; }
std::vector<std::uint8_t> ImageService::readFileRange(const std::string & /*path*/, double /*offset*/, double /*length*/) const { return {}; }
std::vector<std::uint8_t> ImageService::fetchBytes(const std::string & /*url*/) const { return {}; }
std::string ImageService::fetchText(const std::string & /*url*/) const { return {}; }
void ImageService::draw(double id, double x, double y) const
{
	const int slot = integer(id);
	const gea::framework::graphics::pixel::native_t *pixels = imageStore().currentPixels(slot);
	if (!pixels) return;
	gea::platform::display::Display::blitImage(pixels,
	                                           imageStore().currentAlpha(slot),
	                                           imageStore().width(slot),
	                                           imageStore().height(slot),
	                                           integer(x),
	                                           integer(y));
}
double ImageService::width(double id) const { return static_cast<double>(imageStore().width(integer(id))); }
double ImageService::height(double id) const { return static_cast<double>(imageStore().height(integer(id))); }
double ImageService::frameCount(double id) const { return static_cast<double>(imageStore().frameCount(integer(id))); }
bool ImageService::isAnimated(double id) const { return imageStore().isAnimated(integer(id)); }
void ImageService::setPlaying(double id, double playing) const { imageStore().setPlaying(integer(id), playing != 0.0); }
void ImageService::seek(double id, double frame) const { imageStore().seek(integer(id), integer(frame)); }
bool ImageService::advance(double id, double deltaMs) const { return imageStore().advance(integer(id), integer(deltaMs)); }
void ImageService::dispose(double id) const { imageStore().dispose(integer(id)); }
GeaEmbeddedImage ImageService::make(double id) const
{
	const int slot = integer(id);
	GeaEmbeddedImage handle;
	handle.id = slot;
	handle.width = imageStore().width(slot);
	handle.height = imageStore().height(slot);
	handle.frameCount = imageStore().frameCount(slot);
	handle.isAnimated = imageStore().isAnimated(slot);
	return handle;
}

#endif

double Apps::launch(const std::string &appId) const
{
	gLastLaunchedApp = appId;
	return 1.0;
}

}  // namespace gea::host

#ifndef GEA_NATIVE_TEST_REAL_IMAGE_HOST
extern "C" double gea_host_image_load_asset_path(const char *path)
{
	return gea::host::image.loadAssetPath(path);
}

extern "C" double gea_host_image_acquire_asset_path(const char *path, bool *owned)
{
	if (owned) *owned = false;
	return gea::host::image.loadAssetPath(path);
}
#endif

namespace gea::framework::app {
void Application::init(int width, int height, double devicePixelRatio)
{
	gea::embedded::ui::Document::setPreferredMountSize(width, height);
	gea::embedded::ui::setViewportMetrics(width, height, devicePixelRatio);
}
void Application::frame(int timestampMs)
{
	generated::drainMicrotasks();
	gea::framework::display::DisplayBackend::updateAutoRotationFromAccelerometer();
	gea::host::runAnimationFrameCallbacks(static_cast<double>(timestampMs));
	gea::host::websocket::runCallbacks();
	gea::host::rtc::runCallbacks();
	gea::host::http::runRequests();
	gea::embedded::ui::Document::instance().frame(timestampMs);
	generated::drainMicrotasks();
	gea::embedded::ui::Document::instance().refreshMountedIfDirty();
}
void Application::toggleSettings() {}
}  // namespace gea::framework::app

namespace gea::framework::audio {
double AudioBackend::volume() { return 100.0; }
void AudioBackend::setVolume(double /*volume*/) {}
}  // namespace gea::framework::audio

namespace gea::platform::audio {
// The native test harness has no audio device. ui/tree_events.cpp's
// applyAudioAttribute() (always compiled) calls this directly when an <audio>
// node autoplays, so the symbol must resolve at link time. The real
// implementation lives in audio_runtime.cpp, which is ESP-only (unconditional
// FreeRTOS/esp_* includes) and cannot build in the native harness; the rest of
// the audio facade is faked at the gea::host level above.
bool AudioSystem::playFile(const std::string & /*path*/) { return false; }
}  // namespace gea::platform::audio


// `HidBackend`, `WifiBackend` and `GeolocationBackend` are NOT faked here.
// They live in `native_host_backends.cpp`, which every test links -- including
// the app pipeline tests, which exclude this file with
// `GEA_NATIVE_TEST_INCLUDE_HOST=0`. See the note at the top of that file.

namespace gea::framework::display {
double DisplayBackend::brightness() { return static_cast<double>(gea::platform::display::Display::brightness()); }
void DisplayBackend::setBrightness(double brightness) { gea::platform::display::Display::setBrightness(static_cast<int>(brightness)); }
void DisplayBackend::setAA(double samples) { gea::platform::display::Display::setAA(static_cast<int>(samples)); }
void DisplayBackend::setFlushConfig(double rows, double depth)
{
	gea::platform::display::Display::setFlushConfig(static_cast<int>(rows), static_cast<int>(depth));
}
void DisplayBackend::setEpaperRefreshConfig(double fullRefreshEveryPartials,
                                            double fullRefreshHardCapPartials,
                                            double fastStreakWindowMs,
                                            const std::vector<std::uint8_t> &partialLut,
                                            bool hasPartialLut,
                                            const std::vector<std::uint8_t> &fastLut,
                                            bool hasFastLut)
{
	(void)fullRefreshEveryPartials;
	(void)fullRefreshHardCapPartials;
	(void)fastStreakWindowMs;
	(void)partialLut;
	(void)hasPartialLut;
	(void)fastLut;
	(void)hasFastLut;
}
void DisplayBackend::epaperFullRefresh() {}
void DisplayBackend::setEpaperGrayscale(bool enabled) { (void)enabled; }
// Declared at `host/include/host/backends.h:343` and reached from
// `host/include/host/display.h:259`. The real implementation is in
// `host/host/display.cpp`, which this test host does NOT link -- it stands in
// for that whole translation unit, so every method of the contract has to be
// mirrored here. This one was added to the contract and not mirrored, so any
// app whose emit reaches it failed at LINK with an undefined symbol rather
// than at an assertion (`reactive-counter-fonts`, `voice-notes`). Inert, like
// `setEpaperGrayscale` above: there is no e-paper panel behind a native test.
void DisplayBackend::setEpaperFullOnCover(bool enabled) { (void)enabled; }
double DisplayBackend::nativeWidth() { return static_cast<double>(detail::DisplayOrientationState::nativeWidth()); }
double DisplayBackend::nativeHeight() { return static_cast<double>(detail::DisplayOrientationState::nativeHeight()); }
std::string DisplayBackend::orientation() { return detail::DisplayOrientationState::orientationString(); }
void DisplayBackend::setOrientation(const std::string &orientation)
{
	detail::DisplayOrientationState::setOrientation(orientation);
}
std::vector<std::string> DisplayBackend::supportedOrientations()
{
	return detail::DisplayOrientationState::supportedOrientations();
}
void DisplayBackend::setSupportedOrientations(const std::vector<std::string> &orientations)
{
	detail::DisplayOrientationState::setSupportedOrientations(orientations);
}
void DisplayBackend::setSupportedOrientations(const std::string &orientation)
{
	detail::DisplayOrientationState::setSupportedOrientations(orientation);
}
bool DisplayBackend::autoRotate() { return detail::DisplayOrientationState::autoRotate(); }
void DisplayBackend::setAutoRotate(bool enabled) { detail::DisplayOrientationState::setAutoRotate(enabled); }
void DisplayBackend::setVSync(bool on) { gea::platform::display::Display::setVSync(on); }
void DisplayBackend::setTextRasterCache(bool on) { gea::framework::graphics::Canvas::setTextRasterCacheEnabled(on); }
void DisplayBackend::invalidate() { gea::platform::display::Display::invalidate(); }
void DisplayBackend::updateAutoRotationFromAccelerometer()
{
	detail::DisplayOrientationState::updateAutoRotationFromAccelerometer();
}
void DisplayBackend::updateAutoRotation(double acceleration_x, double acceleration_y, double acceleration_z)
{
	detail::DisplayOrientationState::updateAutoRotation(acceleration_x, acceleration_y, acceleration_z);
}
}  // namespace gea::framework::display

namespace gea::platform::display {
void applyOrientation(gea::framework::display::DisplayOrientation /*orientation*/) {}
bool autoRotateAllowed() { return true; }
}  // namespace gea::platform::display

namespace gea::framework::services {
EventQueue FrameScheduler::createEventQueue() { return EventQueue(); }
EventQueue FrameScheduler::eventQueue() { return EventQueue(); }
bool FrameScheduler::sendEvent(const gea::framework::events::Event & /*event*/, int /*wait_ms*/) { return false; }
bool FrameScheduler::receiveEvent(gea::framework::events::Event * /*event*/) { return false; }
void FrameScheduler::start(EventQueue /*queue*/) {}
void FrameScheduler::runFrame(const FrameCallbacks &callbacks)
{
	if (callbacks.frame) callbacks.frame(nowMs(), callbacks.context);
}
void FrameScheduler::setFrameIntervalMs(int interval_ms)
{
	if (interval_ms < kMinFrameIntervalMs) interval_ms = kMinFrameIntervalMs;
	if (interval_ms > kMaxFrameIntervalMs) interval_ms = kMaxFrameIntervalMs;
	gFrameIntervalMs = interval_ms;
}
int FrameScheduler::frameIntervalMs() { return gFrameIntervalMs; }
void FrameScheduler::setFrameRate(double fps)
{
	if (fps <= 0.0) return;
	setFrameIntervalMs(static_cast<int>((1000.0 / fps) + 0.5));
}
double FrameScheduler::frameRate() { return gFrameIntervalMs > 0 ? 1000.0 / static_cast<double>(gFrameIntervalMs) : 0.0; }
int FrameScheduler::nowMs() { return gNowMs; }
}  // namespace gea::framework::services

namespace gea::framework::services {
bool StorageService::init() { return true; }
bool StorageService::getString(const char *key, char *buf, unsigned capacity)
{
	if (!key || !buf || capacity == 0) return false;
	const auto found = gStorage.find(key);
	if (found == gStorage.end()) return false;
	std::snprintf(buf, capacity, "%s", found->second.c_str());
	return true;
}
bool StorageService::setString(const char *key, const char *value)
{
	if (!key) return false;
	gStorage[key] = value ? value : "";
	return true;
}
bool StorageService::loadKv(std::string &out)
{
	out = gStorageKvBlob;
	return !out.empty();
}
void StorageService::saveKv(const std::string &blob)
{
	gStorageKvBlob = blob;
}
}  // namespace gea::framework::services

namespace gea::framework::memory {
void *Allocator::allocatePreferSpiram(std::size_t size, std::size_t alignment)
{
	if (size == 0) size = 1;
	if (alignment <= alignof(std::max_align_t)) return std::malloc(size);
	void *ptr = nullptr;
	return posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
}

void *Allocator::reallocatePreferSpiram(void *ptr, std::size_t size)
{
	return std::realloc(ptr, size);
}

void Allocator::free(void *ptr) noexcept
{
	std::free(ptr);
}

double MemoryBackend::internalFree() { return 1024.0 * 1024.0; }
double MemoryBackend::internalLargestFreeBlock() { return 512.0 * 1024.0; }
double MemoryBackend::internalMinimumFree() { return 512.0 * 1024.0; }
double MemoryBackend::psramFree() { return 4.0 * 1024.0 * 1024.0; }
double MemoryBackend::currentTaskStackHighWaterMark() { return 4096.0; }
double MemoryBackend::geaMainStackBytes() { return 8192.0; }
double MemoryBackend::geaInitStackBytes() { return 8192.0; }
double MemoryBackend::appFrameStackWords() { return 2048.0; }
double MemoryBackend::appFrameStackBytes() { return 8192.0; }
double MemoryBackend::displayFlushConfiguredRows() { return 32.0; }
double MemoryBackend::displayFlushConfiguredDepth() { return 2.0; }
double MemoryBackend::displayFlushBufferMaxBytes() { return 410.0 * 32.0 * 2.0 * 2.0; }
double MemoryBackend::displayFlushRows() { return 32.0; }
double MemoryBackend::displayFlushDepth() { return 2.0; }
double MemoryBackend::displayFlushBufferBytes() { return 410.0 * 32.0 * 2.0 * 2.0; }
double MemoryBackend::allocationSramCount() { return 0.0; }
double MemoryBackend::allocationPsramCount() { return 0.0; }
double MemoryBackend::allocationSramBytes() { return 0.0; }
double MemoryBackend::allocationPsramBytes() { return 0.0; }
double MemoryBackend::allocationSramPeakBytes() { return 0.0; }
double MemoryBackend::allocationPsramPeakBytes() { return 0.0; }
}  // namespace gea::framework::memory


namespace gea::framework::sensors {
void AccelerometerBackend::init() {}
void AccelerometerBackend::close() {}
void AccelerometerBackend::calibrateBias() {}
double AccelerometerBackend::tiltX() { return 0.0; }
double AccelerometerBackend::tiltY() { return 0.0; }
double AccelerometerBackend::accelerationX() { return 0.0; }
double AccelerometerBackend::accelerationY() { return 0.0; }
double AccelerometerBackend::accelerationZ() { return 1.0; }
double AccelerometerBackend::gyroscopeX() { return 0.0; }
double AccelerometerBackend::gyroscopeY() { return 0.0; }
double AccelerometerBackend::gyroscopeZ() { return 0.0; }
}  // namespace gea::framework::sensors

namespace gea::platform::touch {
void Touchscreen::setObserver(Observer /*observer*/) {}
bool Touchscreen::init() { return true; }
int Touchscreen::read(int *x, int *y)
{
	if (x) *x = gTouchX;
	if (y) *y = gTouchY;
	return gTouching ? 1 : 0;
}
int Touchscreen::readCached(int *x, int *y)
{
	if (x) *x = gTouchX;
	if (y) *y = gTouchY;
	return gTouching ? 1 : 0;
}
void Touchscreen::consumeLatestMove(int * /*x*/, int * /*y*/) {}
void Touchscreen::injectEvent(Phase /*phase*/, bool touching, int x, int y)
{
	gTouching = touching;
	gTouchX = x;
	gTouchY = y;
}
}  // namespace gea::platform::touch

namespace gea::host {
struct TouchSample {
	bool touching = false;
	double x = 0;
	double y = 0;
};
class Touch {
public:
	TouchSample read() const;
	TouchSample readRaw() const { return read(); }
};
TouchSample Touch::read() const
{
	int x = 0;
	int y = 0;
	const int touching = gea::platform::touch::Touchscreen::readCached(&x, &y);
	return {touching != 0, static_cast<double>(x), static_cast<double>(y)};
}
}  // namespace gea::host

bool gea::platform::display::Display::init() { return true; }
bool gea::platform::display::Display::start() { return true; }
gea::framework::graphics::Canvas *gea::platform::display::Display::canvas() { return &gDisplayCanvas; }
void gea::platform::display::Display::clear() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::clearNoFlush() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::print(const char * /*text*/) {}
void gea::platform::display::Display::flush()
{
	gFlushCalls++;
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
	if (gDisplayCanvas.dirty(&x0, &y0, &x1, &y1) && x0 <= x1 && y0 <= y1) {
		for (int y = y0; y <= y1; ++y) {
			std::copy_n(&gDisplayPixels[static_cast<std::size_t>(y * gDisplayWidth + x0)],
			            x1 - x0 + 1,
			            &gPresentedPixels[static_cast<std::size_t>(y * gDisplayWidth + x0)]);
		}
		gFlushRects++;
		gFlushPixels += (x1 - x0 + 1) * (y1 - y0 + 1);
		gDisplayCanvas.resetDirty();
	}
}
namespace gea { namespace embedded { namespace test {
// No-op by default. A test driver may set this to observe the exact present
// command stream the app emits (used to repro the device-only incremental
// frame-diff present without flashing hardware).
void (*gPresentObserver)(const gea::platform::display::DisplayPresentCommand *, int) = nullptr;
} } }

bool gea::platform::display::Display::present(const DisplayPresentCommand *commands, int commandCount)
{
	if (!commands || commandCount <= 0) return false;
	if (gea::embedded::test::gPresentObserver) gea::embedded::test::gPresentObserver(commands, commandCount);
	for (int i = 0; i < commandCount; ++i) {
		const auto &command = commands[i];
		switch (command.type) {
			case DisplayPresentCommandType::Clear:
				gDisplayCanvas.clear(command.clear.color);
				break;
			case DisplayPresentCommandType::FillRectRgb565:
				gDisplayCanvas.setGlobalAlpha(command.fillRectRgb565.alpha);
				gDisplayCanvas.fillRect(command.fillRectRgb565.x,
				                        command.fillRectRgb565.y,
				                        command.fillRectRgb565.w,
				                        command.fillRectRgb565.h,
				                        command.fillRectRgb565.color);
				break;
			case DisplayPresentCommandType::StrokeRectRgb565:
				gDisplayCanvas.setGlobalAlpha(command.strokeRectRgb565.alpha);
				gDisplayCanvas.strokeRect(command.strokeRectRgb565.x,
				                          command.strokeRectRgb565.y,
				                          command.strokeRectRgb565.w,
				                          command.strokeRectRgb565.h,
				                          command.strokeRectRgb565.color);
				break;
			case DisplayPresentCommandType::FillTriangleRgb565:
				gDisplayCanvas.setGlobalAlpha(command.fillTriangleRgb565.alpha);
				gDisplayCanvas.fillTriangle(command.fillTriangleRgb565.x0,
				                            command.fillTriangleRgb565.y0,
				                            command.fillTriangleRgb565.x1,
				                            command.fillTriangleRgb565.y1,
				                            command.fillTriangleRgb565.x2,
				                            command.fillTriangleRgb565.y2,
				                            command.fillTriangleRgb565.color);
				break;
			case DisplayPresentCommandType::FillCircleRgb565:
				gDisplayCanvas.setGlobalAlpha(command.fillCircleRgb565.alpha);
				gDisplayCanvas.fillCircle(command.fillCircleRgb565.x,
				                          command.fillCircleRgb565.y,
				                          command.fillCircleRgb565.radius,
				                          command.fillCircleRgb565.color);
				break;
			case DisplayPresentCommandType::StrokeCircleRgb565:
				gDisplayCanvas.setGlobalAlpha(command.strokeCircleRgb565.alpha);
				gDisplayCanvas.strokeCircle(command.strokeCircleRgb565.x,
				                            command.strokeCircleRgb565.y,
				                            command.strokeCircleRgb565.radius,
				                            command.strokeCircleRgb565.color);
				break;
				case DisplayPresentCommandType::FillCirclesRgb565:
					gDisplayCanvas.setGlobalAlpha(command.fillCirclesRgb565.alpha);
					gDisplayCanvas.fillCirclesRgb565(command.fillCirclesRgb565.xs,
					                                 command.fillCirclesRgb565.ys,
					                                 command.fillCirclesRgb565.count,
					                                 command.fillCirclesRgb565.radius,
					                                 command.fillCirclesRgb565.colors);
					break;
				case DisplayPresentCommandType::DrawImage:
					gDisplayCanvas.setGlobalAlpha(command.drawImage.alpha);
					gDisplayCanvas.drawImage(command.drawImage.pixels,
				                         command.drawImage.alphaPixels,
				                         command.drawImage.srcWidth,
				                         command.drawImage.srcHeight,
				                         command.drawImage.x,
				                         command.drawImage.y);
				break;
			case DisplayPresentCommandType::DrawImageScaled:
				gDisplayCanvas.setGlobalAlpha(command.drawImageScaled.alpha);
				gDisplayCanvas.drawImage(command.drawImageScaled.pixels,
				                         command.drawImageScaled.alphaPixels,
				                         command.drawImageScaled.srcWidth,
				                         command.drawImageScaled.srcHeight,
				                         command.drawImageScaled.x,
				                         command.drawImageScaled.y,
				                         command.drawImageScaled.w,
				                         command.drawImageScaled.h);
				break;
			case DisplayPresentCommandType::DrawImageRotated90CW:
				gDisplayCanvas.setGlobalAlpha(command.drawImageRotated90CW.alpha);
				gDisplayCanvas.drawImageRotated90CW(command.drawImageRotated90CW.pixels,
				                                    command.drawImageRotated90CW.alphaPixels,
				                                    command.drawImageRotated90CW.srcWidth,
				                                    command.drawImageRotated90CW.srcHeight,
				                                    command.drawImageRotated90CW.x,
				                                    command.drawImageRotated90CW.y,
				                                    command.drawImageRotated90CW.w,
				                                    command.drawImageRotated90CW.h);
				break;
			case DisplayPresentCommandType::DrawImageTiledX:
				gDisplayCanvas.setGlobalAlpha(command.drawImageTiledX.alpha);
				gDisplayCanvas.drawImageTiledX(command.drawImageTiledX.pixels,
				                               command.drawImageTiledX.alphaPixels,
				                               command.drawImageTiledX.srcWidth,
				                               command.drawImageTiledX.srcHeight,
				                               command.drawImageTiledX.x,
				                               command.drawImageTiledX.y,
				                               command.drawImageTiledX.w);
				break;
			case DisplayPresentCommandType::FillText:
				gDisplayCanvas.setGlobalAlpha(command.fillText.alpha);
				gDisplayCanvas.drawText(command.fillText.text,
				                        command.fillText.x,
				                        command.fillText.y,
				                        command.fillText.color,
				                        command.fillText.scale);
				break;
		}
	}
	gDisplayCanvas.setGlobalAlpha(255);
	flush();
	return true;
}
void gea::platform::display::Display::setFlushConfig(int /*chunk_rows*/, int /*queue_depth*/) {}
int gea::platform::display::Display::flushChunkRows() { return 32; }
int gea::platform::display::Display::flushQueueDepth() { return 2; }
int gea::platform::display::Display::flushBufferBytes() { return 410 * 32 * 2 * 2; }
void gea::platform::display::Display::pushClip(int x, int y, int w, int h) { gDisplayCanvas.pushClip(x, y, w, h); }
void gea::platform::display::Display::popClip() { gDisplayCanvas.popClip(); }
void gea::platform::display::Display::resetClip() { gDisplayCanvas.resetClip(); }
void gea::platform::display::Display::setAlpha(uint8_t alpha)
{
	gDisplayAlpha = alpha;
	gDisplayCanvas.setGlobalAlpha(alpha);
}
uint8_t gea::platform::display::Display::alpha() { return gDisplayAlpha; }
int gea::platform::display::Display::brightness() { return 100; }
void gea::platform::display::Display::setBrightness(int /*brightness_percent*/) {}
bool gea::platform::display::Display::setHighBrightnessMode(bool) { return false; }
bool gea::platform::display::Display::highBrightnessMode() { return false; }
// Tearing sync (TE/VBlank): not implemented on this target.
void gea::platform::display::Display::setVSync(bool) {}
void gea::platform::display::Display::invalidate() {}
bool gea::platform::display::Display::vsyncEnabled() { return false; }
void gea::platform::display::Display::vsyncWaitForFrame() {}
void gea::platform::display::Display::clip(int *x0, int *y0, int *x1, int *y1) { gDisplayCanvas.currentClip(x0, y0, x1, y1); }
void gea::platform::display::Display::fillRect(int x, int y, int w, int h, uint16_t color) { gDisplayCanvas.fillRect(x, y, w, h, color); }
void gea::platform::display::Display::scrollRect(int x, int y, int w, int h, int dx, int dy) { gDisplayCanvas.scrollRect(x, y, w, h, dx, dy); }
void gea::platform::display::Display::resetScrollRegion() { gDisplayCanvas.setScrollRegion(0, 0, 0); }
void gea::platform::display::Display::strokeRect(int x, int y, int w, int h, uint16_t color) { gDisplayCanvas.strokeRect(x, y, w, h, color); }
void gea::platform::display::Display::fillCircle(int cx, int cy, int r, uint16_t color) { gDisplayCanvas.fillCircle(cx, cy, r, color); }
void gea::platform::display::Display::strokeCircle(int cx, int cy, int r, uint16_t color) { gDisplayCanvas.strokeCircle(cx, cy, r, color); }
void gea::platform::display::Display::drawLine(int x0, int y0, int x1, int y1, uint16_t color) { gDisplayCanvas.drawLine(x0, y0, x1, y1, color); }
void gea::platform::display::Display::drawArc(int cx, int cy, int r, int startDeg, int endDeg, uint16_t color) { gDisplayCanvas.drawArc(cx, cy, r, startDeg, endDeg, color); }
void gea::platform::display::Display::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) { gDisplayCanvas.fillTriangle(x0, y0, x1, y1, x2, y2, color); }
void gea::platform::display::Display::drawText(const char *text, int x, int y, uint16_t color, float scale) { gDisplayCanvas.drawText(text, x, y, color, scale); }
void gea::platform::display::Display::drawTextFont(const char *text, int x, int y, uint16_t color, int fontId) { gDisplayCanvas.drawTextFont(text, x, y, color, fontId); }
void gea::platform::display::Display::drawTextFontFamily(const char *text, int x, int y, uint16_t color, int familyId, int sizePx)
{
	gDisplayCanvas.drawTextFontFamily(text, x, y, color, familyId, sizePx);
}
void gea::platform::display::Display::setPixel(int x, int y, uint16_t color) { gDisplayCanvas.fillRect(x, y, 1, 1, color); }
void gea::platform::display::Display::fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, uint16_t color)
{
	gDisplayCanvas.fillRoundedRect(x, y, w, h, tl, tr, br, bl, color);
}
void gea::platform::display::Display::fillRoundedRectBoxesRgb565(const int16_t *xs, const int16_t *ys, int count,
                                                                 int w, int h, int tl, int tr, int br, int bl,
                                                                 const uint16_t *colors)
{
	gDisplayCanvas.fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
}
void gea::platform::display::Display::strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, uint16_t color)
{
	gDisplayCanvas.strokeRoundedRect(x, y, w, h, tl, tr, br, bl, lw, color);
}
void gea::platform::display::Display::blitImage(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int srcW, int srcH, int dx, int dy) { gDisplayCanvas.drawImage(src, alpha, srcW, srcH, dx, dy); }
void gea::platform::display::Display::blitImageScaled(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int srcW, int srcH, int dx, int dy, int dstW, int dstH)
{
	gDisplayCanvas.drawImage(src, alpha, srcW, srcH, dx, dy, dstW, dstH);
}
void gea::platform::display::Display::setWorldOverlay(const uint16_t * /*world_pixels*/, int /*world_width*/, int /*world_height*/, int /*panel_top*/, int /*panel_height*/) {}
void gea::platform::display::Display::setWorldScroll(int /*scroll_x*/) {}
void gea::platform::display::Display::flushStatsRead(int64_t *totalUs, int *callCount, int *pixelCount)
{
	if (totalUs) *totalUs = 0;
	if (callCount) *callCount = gFlushCalls;
	if (pixelCount) *pixelCount = gFlushPixels;
}
gea::platform::display::DisplayFlushPerfStats gea::platform::display::Display::flushPerfStatsRead()
{
	gea::platform::display::DisplayFlushPerfStats stats;
	stats.callCount = gFlushCalls;
	stats.pixelCount = gFlushPixels;
	return stats;
}
void gea::platform::display::Display::flushStatsReset()
{
	gFlushCalls = 0;
	gFlushRects = 0;
	gFlushPixels = 0;
}
const char *gea::platform::display::Display::flushStageName() { return "idle"; }
int gea::platform::display::Display::flushStageChunk() { return 0; }
void gea::platform::display::Display::rebindCanvasToFramebuffer() {
  gDisplayCanvas.bindPixels(gDisplayPixels.data(), gDisplayWidth, gDisplayHeight);
}
void gea::platform::display::Display::flushRectsRasterized(const DisplayFlushRect *rects, int count, DisplayStreamRasterFn raster, void *user, bool) {
  if (!rects || !raster || count <= 0) return;
  ++gFlushCalls;
  for (int i = 0; i < count; ++i) {
    const int x0 = std::max(0, rects[i].x0);
    const int x1 = std::min(gDisplayWidth - 1, rects[i].x1);
    const int y0 = std::max(0, rects[i].y0);
    const int y1 = std::min(gDisplayHeight - 1, rects[i].y1);
    if (x1 < x0 || y1 < y0) continue;
    const int width = x1 - x0 + 1;
    for (int y = y0; y <= y1; y += 16) {
      const int height = std::min(16, y1 - y + 1);
      // DMA staging starts with old garbage, and never updates the PSRAM canvas.
      std::vector<std::uint16_t> chunk(width * height, 0xa55a);
      raster(chunk.data(), width, height, x0, y, user);
      for (int row = 0; row < height; ++row)
        std::copy_n(chunk.data() + row * width, width,
                    gPresentedPixels.data() + (y + row) * gDisplayWidth + x0);
      rebindCanvasToFramebuffer();
    }
    ++gFlushRects;
    gFlushPixels += width * (y1 - y0 + 1);
  }
  gDisplayCanvas.resetDirty();
}
void gea::platform::display::Display::flushRects(const gea::platform::display::DisplayFlushRect *rects, int count, bool)
{
	if (!rects || count <= 0) return;
	gFlushCalls++;
	for (int i = 0; i < count; i++) {
		int x0 = rects[i].x0;
		int y0 = rects[i].y0;
		int x1 = rects[i].x1;
		int y1 = rects[i].y1;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 >= gDisplayWidth) x1 = gDisplayWidth - 1;
		if (y1 >= gDisplayHeight) y1 = gDisplayHeight - 1;
		if (x0 <= x1 && y0 <= y1) {
			for (int y = y0; y <= y1; ++y) {
				std::copy_n(&gDisplayPixels[static_cast<std::size_t>(y * gDisplayWidth + x0)],
				            x1 - x0 + 1,
				            &gPresentedPixels[static_cast<std::size_t>(y * gDisplayWidth + x0)]);
			}
			gFlushRects++;
			gFlushPixels += (x1 - x0 + 1) * (y1 - y0 + 1);
		}
	}
	gDisplayCanvas.resetDirty();
}
bool gea::platform::display::Display::streamRect(int x, int y, int w, int h, DisplayStreamRasterFn raster, void *user)
{
	if (!raster || w <= 0 || h <= 0) return false;
	int x0 = x;
	int y0 = y;
	int x1 = x + w - 1;
	int y1 = y + h - 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= gDisplayWidth) x1 = gDisplayWidth - 1;
	if (y1 >= gDisplayHeight) y1 = gDisplayHeight - 1;
	if (x0 > x1 || y0 > y1) return true;

	const int width = x1 - x0 + 1;
	const int height = y1 - y0 + 1;
	std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width * height));
	raster(pixels.data(), width, height, x0, y0, user);
	for (int row = 0; row < height; row++) {
		std::copy_n(&pixels[static_cast<std::size_t>(row * width)],
		            width,
		            &gDisplayPixels[static_cast<std::size_t>((y0 + row) * gDisplayWidth + x0)]);
		std::copy_n(&pixels[static_cast<std::size_t>(row * width)],
		            width,
		            &gPresentedPixels[static_cast<std::size_t>((y0 + row) * gDisplayWidth + x0)]);
	}
	gFlushCalls++;
	gFlushRects++;
	gFlushPixels += width * height;
	gDisplayCanvas.resetDirty();
	return true;
}
