// Test harness for canvas-3d compiled via:
// gea IR backend -> geatsc -> native retained UI -> display-backed canvas.

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include <unistd.h>

#include "audio.h"
#include "canvas.h"
#include "display.h"
#include "events.h"
#include "host/audio.h"
#include "host/timers.h"
#include "memory.h"
#include "pixel.h"
#include "services/frame_scheduler.h"
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"

extern void __gea_top_level();
namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

namespace {
constexpr int kDisplayPixels = gea::platform::display::kWidth * gea::platform::display::kHeight;

gea::framework::graphics::Canvas gDisplayCanvas;
std::vector<std::uint16_t> gDisplayPixels(static_cast<std::size_t>(kDisplayPixels));
std::vector<std::uint16_t> gPanelPixels(static_cast<std::size_t>(kDisplayPixels));
std::uint8_t gDisplayAlpha = 255;
int gFlushCallCount = 0;
int gFlushPixelCount = 0;
int gFrameIntervalMs = gea::framework::services::FrameScheduler::kDefaultFrameIntervalMs;

struct PixelStats {
  int nonzero = 0;
  int changed = 0;
  int bright = 0;
  std::size_t unique = 0;
};

void unpack_panel_pixel(std::uint16_t pixel, int *r, int *g, int *b) {
  const std::uint16_t rgb565 = gea::framework::graphics::pixel::toRgb565(pixel);
  const int r5 = (rgb565 >> 11) & 0x1f;
  const int g6 = (rgb565 >> 5) & 0x3f;
  const int b5 = rgb565 & 0x1f;
  *r = (r5 * 255) / 31;
  *g = (g6 * 255) / 63;
  *b = (b5 * 255) / 31;
}

void fail_on_alarm(int /*signal*/) {
  std::fputs("[test_gea_canvas_3d_main] timed out while compiling/running canvas-3d\n", stderr);
  std::_Exit(124);
}

void reset_display_pixels() {
  std::fill(gDisplayPixels.begin(), gDisplayPixels.end(), static_cast<std::uint16_t>(0));
  std::fill(gPanelPixels.begin(), gPanelPixels.end(), static_cast<std::uint16_t>(0));
  gFlushCallCount = 0;
  gFlushPixelCount = 0;
  gDisplayCanvas.bindPixels(gDisplayPixels.data(), gea::platform::display::kWidth, gea::platform::display::kHeight);
}

PixelStats pixel_stats(const std::vector<std::uint16_t> *previous = nullptr) {
  std::set<std::uint16_t> unique;
  PixelStats stats;
  for (std::size_t i = 0; i < gPanelPixels.size(); i++) {
    const auto pixel = gPanelPixels[i];
    if (pixel != 0) stats.nonzero++;
    unique.insert(pixel);
    int r = 0;
    int g = 0;
    int b = 0;
    unpack_panel_pixel(pixel, &r, &g, &b);
    if (r + g + b > 180) stats.bright++;
    if (previous && i < previous->size() && (*previous)[i] != pixel) stats.changed++;
  }
  stats.unique = unique.size();
  return stats;
}

int find_canvas_descendant(int root) {
  auto &tree = gea::embedded::ui::Tree::instance();
  if (root < 0 || root >= tree.nodeCount()) return -1;
  if (tree.node(root).type == gea::embedded::ui::NodeType::Canvas) return root;
  for (int child = tree.node(root).first_child; child >= 0; child = tree.node(child).next_sibling) {
    const int match = find_canvas_descendant(child);
    if (match >= 0) return match;
  }
  return -1;
}

void copy_dirty_display_to_panel() {
  int x0 = 0;
  int y0 = 0;
  int x1 = -1;
  int y1 = -1;
  if (!gDisplayCanvas.dirty(&x0, &y0, &x1, &y1)) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= gea::platform::display::kWidth) x1 = gea::platform::display::kWidth - 1;
  if (y1 >= gea::platform::display::kHeight) y1 = gea::platform::display::kHeight - 1;
  if (x0 > x1 || y0 > y1) return;

  for (int y = y0; y <= y1; y++) {
    const auto offset = static_cast<std::size_t>(y * gea::platform::display::kWidth + x0);
    std::copy(gDisplayPixels.begin() + static_cast<std::ptrdiff_t>(offset),
              gDisplayPixels.begin() + static_cast<std::ptrdiff_t>(offset + x1 - x0 + 1),
              gPanelPixels.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  gFlushCallCount++;
  gFlushPixelCount += (x1 - x0 + 1) * (y1 - y0 + 1);
  gDisplayCanvas.resetDirty();
}

void write_panel_ppm() {
#ifdef GEA_CANVAS_3D_TEST_PANEL_PPM
  auto *file = std::fopen(GEA_CANVAS_3D_TEST_PANEL_PPM, "wb");
  if (!file) return;
  std::fprintf(file, "P6\n%d %d\n255\n", gea::platform::display::kWidth, gea::platform::display::kHeight);
  for (std::uint16_t pixel : gPanelPixels) {
    int r = 0;
    int g = 0;
    int b = 0;
    unpack_panel_pixel(pixel, &r, &g, &b);
    const unsigned char rgb[3] = {
      static_cast<unsigned char>(r),
      static_cast<unsigned char>(g),
      static_cast<unsigned char>(b),
    };
    std::fwrite(rgb, 1, sizeof(rgb), file);
  }
  std::fclose(file);
#endif
}

bool expect_painted_scene(const char *label, const std::vector<std::uint16_t> *previous = nullptr) {
  write_panel_ppm();
  const auto stats = pixel_stats(previous);
  if (stats.nonzero < kDisplayPixels - 256 || stats.unique < 6 || stats.bright < 512) {
    std::fprintf(
        stderr,
        "[test_gea_canvas_3d_main] expected visible %s scene, nonzero=%d/%d unique=%zu bright=%d changed=%d\n",
        label,
        stats.nonzero,
        kDisplayPixels,
        stats.unique,
        stats.bright,
        stats.changed);
    return false;
  }
  if (previous && stats.changed < 128) {
    std::fprintf(
        stderr,
        "[test_gea_canvas_3d_main] expected %s frame to change pixels, changed=%d unique=%zu\n",
        label,
        stats.changed,
        stats.unique);
    return false;
  }
  return true;
}

bool dispatch_touch_start(int target, int x, int y) {
  gea::framework::events::PointerEvent event{gea::framework::events::PointerEventType::TouchStart};
  event.targetId = target;
  event.x = x;
  event.y = y;
  event.clientX = x;
  event.clientY = y;
  event.pageX = x;
  event.pageY = y;
  event.screenX = x;
  event.screenY = y;
  event.touchesLength = 1;
  event.targetTouchesLength = 1;
  event.changedTouchesLength = 1;
  event.touches[0].target = gea::framework::events::EventTarget(target);
  event.touches[0].clientX = x;
  event.touches[0].clientY = y;
  event.targetTouches[0] = event.touches[0];
  event.changedTouches[0] = event.touches[0];
  return gea::embedded::ui::Tree::instance().dispatchEvent(event);
}
}  // namespace

extern "C" double gea_host_image_load_asset_path(const char * /*path*/) {
  return -1.0;
}

namespace gea::host {
AudioDestinationProperty::operator AudioDestinationNode() const {
  return AudioDestinationNode(1.0);
}

AudioDestinationProperty::operator double() const {
  return 1.0;
}

AudioContextCurrentTimeProperty::operator double() const {
  return 0.0;
}

const AudioParamValueProperty &AudioParamValueProperty::operator=(double /*frequency_hz*/) const {
  return *this;
}

AudioParamValueProperty::operator double() const {
  return 440.0;
}

const AudioParam &AudioParam::operator=(double /*frequency_hz*/) const {
  return *this;
}

void AudioParam::setValueAtTime(double /*frequency_hz*/, double /*start_time*/) const {}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(double /*type*/) const {
  return *this;
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const char * /*type*/) const {
  return *this;
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const std::string & /*type*/) const {
  return *this;
}

void OscillatorNode::connect(AudioDestinationNode /*destination*/) const {}
void OscillatorNode::connect(AudioDestinationProperty /*destination*/) const {}
void OscillatorNode::connect(double /*destinationHandle*/) const {}
void OscillatorNode::start(double /*when*/) const {}
void OscillatorNode::stop(double /*when*/) const {}

OscillatorNode AudioContext::createOscillator() const {
  return OscillatorNode(2.0);
}

struct HTMLAudioElement::State {
  int nodeId = -1;
};
HTMLAudioElement::HTMLAudioElement(const gea::embedded::ui::NodeHandle &node) : state_(std::make_shared<State>()) { state_->nodeId = node.id(); }
bool HTMLAudioElement::play() const { return true; }
void HTMLAudioElement::pause() const {}
}  // namespace gea::host

namespace gea::framework::audio {
double AudioBackend::volume() {
  return 100.0;
}

void AudioBackend::setVolume(double /*volume*/) {}
}  // namespace gea::framework::audio

namespace gea::platform::audio {
// No audio device in the native test harness; ui/tree_events.cpp's
// applyAudioAttribute() (always compiled) calls this directly. The real
// implementation (audio_runtime.cpp) is ESP-only and cannot build natively.
bool AudioSystem::playFile(const std::string & /*path*/) { return false; }
}  // namespace gea::platform::audio

namespace gea::framework::display {
double DisplayBackend::brightness() {
  return static_cast<double>(gea::platform::display::Display::brightness());
}

void DisplayBackend::setBrightness(double brightness) {
  gea::platform::display::Display::setBrightness(static_cast<int>(brightness));
}

void DisplayBackend::setFlushConfig(double rows, double depth) {
  gea::platform::display::Display::setFlushConfig(static_cast<int>(rows), static_cast<int>(depth));
}

void DisplayBackend::setVSync(bool on) {
  gea::platform::display::Display::setVSync(on);
}
void DisplayBackend::setTextRasterCache(bool on) {
  gea::framework::graphics::Canvas::setTextRasterCacheEnabled(on);
}
void DisplayBackend::invalidate() {
  gea::platform::display::Display::invalidate();
}
}  // namespace gea::framework::display

namespace gea::framework::memory {
void *Allocator::allocatePreferSpiram(std::size_t size, std::size_t alignment) {
  if (size == 0) size = 1;
  if (alignment <= alignof(std::max_align_t)) return std::malloc(size);
  void *ptr = nullptr;
  return posix_memalign(&ptr, alignment, size) == 0 ? ptr : nullptr;
}

void *Allocator::reallocatePreferSpiram(void *ptr, std::size_t size) {
  return std::realloc(ptr, size);
}

void Allocator::free(void *ptr) noexcept {
  std::free(ptr);
}
}  // namespace gea::framework::memory

namespace gea::framework::services {
EventQueue FrameScheduler::createEventQueue() { return EventQueue(); }
EventQueue FrameScheduler::eventQueue() { return EventQueue(); }
bool FrameScheduler::sendEvent(const gea::framework::events::Event & /*event*/, int /*wait_ms*/) { return false; }
bool FrameScheduler::receiveEvent(gea::framework::events::Event * /*event*/) { return false; }
void FrameScheduler::start(EventQueue /*queue*/) {}
void FrameScheduler::runFrame(const FrameCallbacks &callbacks) {
  if (callbacks.frame) callbacks.frame(nowMs(), callbacks.context);
}
void FrameScheduler::setFrameIntervalMs(int interval_ms) {
  if (interval_ms < kMinFrameIntervalMs) interval_ms = kMinFrameIntervalMs;
  if (interval_ms > kMaxFrameIntervalMs) interval_ms = kMaxFrameIntervalMs;
  gFrameIntervalMs = interval_ms;
}
int FrameScheduler::frameIntervalMs() { return gFrameIntervalMs; }
void FrameScheduler::setFrameRate(double fps) {
  if (fps <= 0.0) return;
  setFrameIntervalMs(static_cast<int>((1000.0 / fps) + 0.5));
}
double FrameScheduler::frameRate() { return gFrameIntervalMs > 0 ? 1000.0 / static_cast<double>(gFrameIntervalMs) : 0.0; }
int FrameScheduler::nowMs() { return 0; }
}  // namespace gea::framework::services

bool gea::platform::display::Display::init() { return true; }
bool gea::platform::display::Display::start() { return true; }
gea::framework::graphics::Canvas *gea::platform::display::Display::canvas() { return &gDisplayCanvas; }
void gea::platform::display::Display::clear() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::clearNoFlush() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::print(const char * /*text*/) {}
void gea::platform::display::Display::flush() { copy_dirty_display_to_panel(); }
bool gea::platform::display::Display::present(const DisplayPresentCommand *commands, int command_count) {
  if (!commands || command_count <= 0) return false;
  for (int i = 0; i < command_count; i++) {
    const DisplayPresentCommand &command = commands[i];
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
  gea::platform::display::Display::flush();
  return true;
}
void gea::platform::display::Display::setFlushConfig(int /*chunk_rows*/, int /*queue_depth*/) {}
void gea::platform::display::Display::pushClip(int x, int y, int w, int h) { gDisplayCanvas.pushClip(x, y, w, h); }
void gea::platform::display::Display::popClip() { gDisplayCanvas.popClip(); }
void gea::platform::display::Display::resetClip() { gDisplayCanvas.resetClip(); }
void gea::platform::display::Display::setAlpha(uint8_t alpha) {
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
void gea::platform::display::Display::drawArc(int cx, int cy, int r, int start_deg, int end_deg, uint16_t color) { gDisplayCanvas.drawArc(cx, cy, r, start_deg, end_deg, color); }
void gea::platform::display::Display::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) { gDisplayCanvas.fillTriangle(x0, y0, x1, y1, x2, y2, color); }
void gea::platform::display::Display::drawText(const char *text, int x, int y, uint16_t color, float scale) { gDisplayCanvas.drawText(text, x, y, color, scale); }
void gea::platform::display::Display::drawTextFont(const char *text, int x, int y, uint16_t color, int font_id) { gDisplayCanvas.drawTextFont(text, x, y, color, font_id); }
void gea::platform::display::Display::drawTextFontFamily(const char *text, int x, int y, uint16_t color, int /*family_id*/, int size_px) {
  gDisplayCanvas.drawText(text, x, y, color, static_cast<float>(size_px) / 16.0f);
}
void gea::platform::display::Display::setPixel(int x, int y, uint16_t color) { gDisplayCanvas.fillRect(x, y, 1, 1, color); }
void gea::platform::display::Display::fillRoundedRect(int x, int y, int w, int h, int /*tl*/, int /*tr*/, int /*br*/, int /*bl*/, uint16_t color) { gDisplayCanvas.fillRect(x, y, w, h, color); }
void gea::platform::display::Display::fillRoundedRectBoxesRgb565(const int16_t *xs, const int16_t *ys, int count,
                                                                 int w, int h, int tl, int tr, int br, int bl,
                                                                 const uint16_t *colors) {
  gDisplayCanvas.fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
}
void gea::platform::display::Display::strokeRoundedRect(int x, int y, int w, int h, int /*tl*/, int /*tr*/, int /*br*/, int /*bl*/, int /*lw*/, uint16_t color) { gDisplayCanvas.strokeRect(x, y, w, h, color); }
void gea::platform::display::Display::blitImage(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy) { gDisplayCanvas.drawImage(src, alpha, src_w, src_h, dx, dy); }
void gea::platform::display::Display::blitImageScaled(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy, int dst_w, int dst_h) {
  gDisplayCanvas.drawImage(src, alpha, src_w, src_h, dx, dy, dst_w, dst_h);
}
void gea::platform::display::Display::setWorldOverlay(const uint16_t * /*world_pixels*/, int /*world_width*/, int /*world_height*/, int /*panel_top*/, int /*panel_height*/) {}
void gea::platform::display::Display::setWorldScroll(int /*scroll_x*/) {}
void gea::platform::display::Display::flushStatsRead(int64_t *total_us, int *call_count, int *pixel_count) {
  if (total_us) *total_us = 0;
  if (call_count) *call_count = gFlushCallCount;
  if (pixel_count) *pixel_count = gFlushPixelCount;
}
gea::platform::display::DisplayFlushPerfStats gea::platform::display::Display::flushPerfStatsRead() { return {}; }
void gea::platform::display::Display::flushStatsReset() {
  gFlushCallCount = 0;
  gFlushPixelCount = 0;
}
const char *gea::platform::display::Display::flushStageName() { return "idle"; }
int gea::platform::display::Display::flushStageChunk() { return 0; }
void gea::platform::display::Display::flushRects(const gea::platform::display::DisplayFlushRect *, int, bool) {
  gea::platform::display::Display::flush();
}
bool gea::platform::display::Display::streamRect(int, int, int, int, DisplayStreamRasterFn, void *) {
  return false;
}

int main() {
  std::signal(SIGALRM, fail_on_alarm);
  alarm(5);

  reset_display_pixels();
  __gea_top_level();
  gea::framework::app::generated::drainMicrotasks();
  gea::embedded::ui::Document::instance().refreshMountedIfDirty();

  auto &tree = gea::embedded::ui::Tree::instance();
  const int root = tree.mountedRoot();
  const int canvas = find_canvas_descendant(root);
  if (canvas < 0) {
    std::fprintf(stderr, "[test_gea_canvas_3d_main] expected mounted canvas descendant, got root=%d\n", root);
    return 1;
  }
  const auto &canvasNode = tree.node(canvas);
  if (canvasNode.layout.width < gea::platform::display::kWidth || canvasNode.layout.height < gea::platform::display::kHeight) {
    std::fprintf(stderr,
                 "[test_gea_canvas_3d_main] expected fullscreen canvas layout, got %dx%d\n",
                 canvasNode.layout.width,
                 canvasNode.layout.height);
    return 1;
  }
  if (!expect_painted_scene("initial")) return 1;

  if (gFlushCallCount <= 0 || gFlushPixelCount <= 0) {
    std::fprintf(stderr,
                 "[test_gea_canvas_3d_main] expected initial frame to flush to panel, flushes=%d pixels=%d\n",
                 gFlushCallCount,
                 gFlushPixelCount);
    return 1;
  }

  const auto before_frame = gPanelPixels;
  gea::host::runAnimationFrameCallbacks(16.67);
  gea::framework::app::generated::drainMicrotasks();
  gea::embedded::ui::Document::instance().refreshMountedIfDirty();
  gea::host::runAnimationFrameCallbacks(33.34);
  gea::framework::app::generated::drainMicrotasks();
  gea::embedded::ui::Document::instance().refreshMountedIfDirty();
  if (!expect_painted_scene("animated", &before_frame)) return 1;

  alarm(0);
  return 0;
}
