// Test harness for bouncing-balls-jsx compiled via:
// gea IR backend -> geatsc -> native retained UI.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "canvas.h"
#include "display.h"
#include "pixel.h"
#include "host/audio.h"
#include "host/backends.h"
#include "host/timers.h"
#include "memory.h"
#include "services/frame_scheduler.h"
#include "ui/node.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/style.h"
#include "ui/tree_state.h"
#include "ui/tree_internal.h"

extern void __gea_top_level();
extern "C" double gea_host_image_load_asset_path(const char * /*path*/) { return 0.0; }
namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

namespace {
gea::framework::graphics::Canvas gDisplayCanvas;
std::vector<std::uint16_t> gDisplayPixels;
std::vector<std::uint16_t> gPanelPixels;
std::vector<std::uint16_t> gBackdropPixels;
std::uint8_t gDisplayAlpha = 255;
int gFlushCallCount = 0;
int gFlushPixelCount = 0;
int gFrameIntervalMs = gea::framework::services::FrameScheduler::kDefaultFrameIntervalMs;
int gRoundedRectCallCount = 0;
int gRoundedRectBatchCallCount = 0;
int gRoundedRectBatchItemCount = 0;
int gFlushRectCallCount = 0;
int gFlushRectItemCount = 0;
int gFullWidthFlushRectItemCount = 0;
long long gFlushRectPixelCount = 0;
int gMaxFlushRectPixels = 0;
int gMaxFlushRectWidth = 0;
int gMaxPartialFlushRectWidth = 0;
// Largest single Display::flush() dirty bounding box seen during the animation.
// The simpleUnifiedReplay path flushes via flush() (not flushRects), so this is
// the only place that captures whether scattered ball rects are flushed sparsely
// (per-rect, small bbox each) or coalesced into one full-surface union flush
// (the afbf1c7b regression). Canvas::kMaxDirtyRects=24, so a deferred union flush
// of ~25 ball rects overflows the canvas dirty tracker and collapses to a near
// full-surface bbox here, exactly as it does on hardware.
long long gMaxFlushDirtyBboxPixels = 0;
int gTextDrawCallCount = 0;
int gFpsTextDrawCallCount = 0;
int gCurrentFrameTextDrawCallCount = 0;
int gFramesWithFpsTextReplay = 0;
int gFpsTextChanges = 0;
std::string gLastFpsText;
constexpr int kExpectedBallCount = 64;
constexpr int kExpectedFpsFontSize = 187;  // 26vmin at the 720x1440 host viewport.

bool expect_contains(const std::string &haystack, const char *needle, const char *label) {
  if (haystack.find(needle) != std::string::npos) return true;
  std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected %s to contain %s, got:\n%s\n", label, needle, haystack.c_str());
  return false;
}

extern "C" std::uint16_t *gea_backdrop_cache(int *cap_px) {
  constexpr int kCapacity = 720 * 1440;
  if (gBackdropPixels.size() < kCapacity) gBackdropPixels.assign(kCapacity, 0);
  if (cap_px) *cap_px = kCapacity;
  return gBackdropPixels.data();
}

void fail_on_alarm(int /*signal*/) {
  std::fputs("[test_gea_bouncing_balls_jsx_main] timed out while driving bouncing-balls frames\n", stderr);
  std::_Exit(124);
}

void append_text(int id, std::string &out) {
  auto &tree = gea::embedded::ui::Tree::instance();
  if (id < 0 || id >= tree.nodeCount()) return;
  const auto &node = tree.node(id);
  if (node.text[0] != '\0') out += node.text;
  for (int child = node.first_child; child >= 0; child = tree.node(child).next_sibling) append_text(child, out);
}

std::string root_text_content() {
  std::string out;
  append_text(gea::embedded::ui::Tree::instance().mountedRoot(), out);
  return out;
}

std::size_t count_nodes_with_class(const std::string &class_name) {
  std::size_t count = 0;
  auto &tree = gea::embedded::ui::Tree::instance();
  for (int id = 0; id < tree.nodeCount(); id++) {
    if (tree.hasClass(id, class_name)) count++;
  }
  return count;
}

int first_node_with_class(const std::string &class_name) {
  auto &tree = gea::embedded::ui::Tree::instance();
  for (int id = 0; id < tree.nodeCount(); id++) {
    if (tree.hasClass(id, class_name)) return id;
  }
  return -1;
}

void dump_tree();

bool expect_node_size(const char *class_name, int expected_width, int expected_height) {
  const int id = first_node_with_class(class_name);
  if (id < 0) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected retained JSX %s node\n", class_name);
    dump_tree();
    return false;
  }
  const auto &node = gea::embedded::ui::Tree::instance().node(id);
  if (node.layout.width != expected_width || node.layout.height != expected_height) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected %s to fill viewport %dx%d, got %dx%d\n",
                 class_name,
                 expected_width,
                 expected_height,
                 node.layout.width,
                 node.layout.height);
    dump_tree();
    return false;
  }
  return true;
}

int bright_pixels_in_rect(int x0, int y0, int x1, int y1) {
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(719, x1);
  y1 = std::min(1439, y1);
  if (x0 > x1 || y0 > y1 || gDisplayPixels.empty()) return 0;
  int count = 0;
  for (int y = y0; y <= y1; y++) {
    const std::uint16_t *row = gDisplayPixels.data() + static_cast<std::size_t>(y) * 720;
    for (int x = x0; x <= x1; x++) {
      int r = 0;
      int g = 0;
      int b = 0;
      gea::framework::graphics::pixel::unpackRgb565(row[x], &r, &g, &b);
      if (r >= 24 && g >= 48 && b >= 24) count++;
    }
  }
  return count;
}

bool write_display_ppm(const char *path) {
  if (!path || path[0] == '\0') return true;
  if (gDisplayPixels.size() != static_cast<std::size_t>(720 * 1440)) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] cannot dump display snapshot before panel allocation\n");
    return false;
  }

  FILE *file = std::fopen(path, "wb");
  if (!file) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] failed to open display snapshot %s: %s\n",
                 path,
                 std::strerror(errno));
    return false;
  }

  std::fprintf(file, "P6\n720 1440\n255\n");
  for (std::uint16_t pixel : gDisplayPixels) {
    int r = 0;
    int g = 0;
    int b = 0;
    gea::framework::graphics::pixel::unpackRgb565(pixel, &r, &g, &b);
    std::fputc(r, file);
    std::fputc(g, file);
    std::fputc(b, file);
  }
  const bool ok = std::fclose(file) == 0;
  if (!ok) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] failed to write display snapshot %s: %s\n",
                 path,
                 std::strerror(errno));
  } else {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] wrote display snapshot %s\n", path);
  }
  return ok;
}

void ensure_panel_pixels() {
  constexpr int kWidth = 720;
  constexpr int kHeight = 1440;
  if (gPanelPixels.size() != static_cast<std::size_t>(kWidth * kHeight)) {
    gPanelPixels.assign(static_cast<std::size_t>(kWidth * kHeight), 0);
  }
}

void copy_display_to_panel_rect(int x0, int y0, int x1, int y1) {
  ensure_panel_pixels();
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(719, x1);
  y1 = std::min(1439, y1);
  if (x0 > x1 || y0 > y1) return;
  for (int y = y0; y <= y1; y++) {
    const std::uint16_t *src = gDisplayPixels.data() + static_cast<std::size_t>(y) * 720 + x0;
    std::uint16_t *dst = gPanelPixels.data() + static_cast<std::size_t>(y) * 720 + x0;
    std::memcpy(dst, src, static_cast<std::size_t>(x1 - x0 + 1) * sizeof(std::uint16_t));
  }
}

void copy_display_dirty_to_panel() {
  gea::framework::graphics::CanvasDirtyRect rects[gea::framework::graphics::Canvas::kMaxDirtyRects];
  const int count = gDisplayCanvas.dirtyRects(rects, gea::framework::graphics::Canvas::kMaxDirtyRects);
  if (count > 0) {
    for (int i = 0; i < count; i++) {
      copy_display_to_panel_rect(rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1);
    }
    return;
  }
  int x0 = 0;
  int y0 = 0;
  int x1 = -1;
  int y1 = -1;
  if (gDisplayCanvas.dirty(&x0, &y0, &x1, &y1)) copy_display_to_panel_rect(x0, y0, x1, y1);
}

bool expect_incremental_frame_matches_full_replay(const char *phase, int frame) {
  std::vector<std::uint16_t> incremental = gDisplayPixels;
  std::vector<std::uint16_t> full(static_cast<std::size_t>(720 * 1440), 0);
  const int savedFlushPixelCount = gFlushPixelCount;
  const int savedRoundedRectCallCount = gRoundedRectCallCount;
  const int savedRoundedRectBatchCallCount = gRoundedRectBatchCallCount;
  const int savedRoundedRectBatchItemCount = gRoundedRectBatchItemCount;
  const int savedTextDrawCallCount = gTextDrawCallCount;
  const int savedFpsTextDrawCallCount = gFpsTextDrawCallCount;
  const int savedCurrentFrameTextDrawCallCount = gCurrentFrameTextDrawCallCount;
  const int savedFpsTextChanges = gFpsTextChanges;
  const std::string savedLastFpsText = gLastFpsText;

  gDisplayCanvas.bindPixels(full.data(), 720, 1440);
  gDisplayCanvas.resetClip();
  gDisplayCanvas.setGlobalAlpha(255);
  gDisplayCanvas.clear(0);
  gDisplayCanvas.resetDirty();
  gea::embedded::ui::DisplayList::instance().replay();

  gDisplayCanvas.bindPixels(gDisplayPixels.data(), 720, 1440);
  gDisplayCanvas.resetClip();
  gDisplayCanvas.setGlobalAlpha(gDisplayAlpha);
  gDisplayCanvas.resetDirty();
  gFlushPixelCount = savedFlushPixelCount;
  gRoundedRectCallCount = savedRoundedRectCallCount;
  gRoundedRectBatchCallCount = savedRoundedRectBatchCallCount;
  gRoundedRectBatchItemCount = savedRoundedRectBatchItemCount;
  gTextDrawCallCount = savedTextDrawCallCount;
  gFpsTextDrawCallCount = savedFpsTextDrawCallCount;
  gCurrentFrameTextDrawCallCount = savedCurrentFrameTextDrawCallCount;
  gFpsTextChanges = savedFpsTextChanges;
  gLastFpsText = savedLastFpsText;

  int mismatches = 0;
  int firstX = -1;
  int firstY = -1;
  std::uint16_t firstIncremental = 0;
  std::uint16_t firstFull = 0;
  for (int y = 0; y < 1440; y++) {
    for (int x = 0; x < 720; x++) {
      const std::size_t offset = static_cast<std::size_t>(y) * 720 + x;
      if (incremental[offset] == full[offset]) continue;
      if (mismatches == 0) {
        firstX = x;
        firstY = y;
        firstIncremental = incremental[offset];
        firstFull = full[offset];
      }
      mismatches++;
    }
  }
  if (mismatches == 0) return true;
  std::fprintf(stderr,
               "[test_gea_bouncing_balls_jsx_main] retained incremental frame differs from full replay after %s frame=%d: mismatches=%d first=(%d,%d) incremental=0x%04x full=0x%04x\n",
               phase,
               frame,
               mismatches,
               firstX,
               firstY,
               firstIncremental,
               firstFull);
  auto &tree = gea::embedded::ui::Tree::instance();
  for (int id = 0; id < tree.nodeCount(); id++) {
    if (!tree.hasClass(id, "ball")) continue;
    const auto &node = tree.node(id);
    const int nx0 = node.layout.x - 2;
    const int ny0 = node.layout.y - 2;
    const int nx1 = node.layout.x + node.layout.width + 1;
    const int ny1 = node.layout.y + node.layout.height + 1;
    if (firstX < nx0 || firstX > nx1 || firstY < ny0 || firstY > ny1) continue;
    std::fprintf(stderr,
                 "  nearby ball node=%d current=(%d,%d %dx%d) previous=(%d,%d %dx%d) dirty=%u commandDirty=%u\n",
                 id,
                 node.layout.x,
                 node.layout.y,
                 node.layout.width,
                 node.layout.height,
                 node.layout.previous_x,
                 node.layout.previous_y,
                 node.layout.previous_width,
                 node.layout.previous_height,
                 static_cast<unsigned>(node.render.dirty),
                 static_cast<unsigned>(gea::embedded::ui::treeState().nodeCommandDirty[id]));
  }
  return false;
}

bool expect_presented_panel_matches_full_replay(const char *phase, int frame) {
  ensure_panel_pixels();
  std::vector<std::uint16_t> panel = gPanelPixels;
  std::vector<std::uint16_t> full(static_cast<std::size_t>(720 * 1440), 0);
  const int savedFlushPixelCount = gFlushPixelCount;
  const int savedRoundedRectCallCount = gRoundedRectCallCount;
  const int savedRoundedRectBatchCallCount = gRoundedRectBatchCallCount;
  const int savedRoundedRectBatchItemCount = gRoundedRectBatchItemCount;
  const int savedTextDrawCallCount = gTextDrawCallCount;
  const int savedFpsTextDrawCallCount = gFpsTextDrawCallCount;
  const int savedCurrentFrameTextDrawCallCount = gCurrentFrameTextDrawCallCount;
  const int savedFpsTextChanges = gFpsTextChanges;
  const std::string savedLastFpsText = gLastFpsText;

  gDisplayCanvas.bindPixels(full.data(), 720, 1440);
  gDisplayCanvas.resetClip();
  gDisplayCanvas.setGlobalAlpha(255);
  gDisplayCanvas.clear(0);
  gDisplayCanvas.resetDirty();
  gea::embedded::ui::DisplayList::instance().replay();

  gDisplayCanvas.bindPixels(gDisplayPixels.data(), 720, 1440);
  gDisplayCanvas.resetClip();
  gDisplayCanvas.setGlobalAlpha(gDisplayAlpha);
  gDisplayCanvas.resetDirty();
  gFlushPixelCount = savedFlushPixelCount;
  gRoundedRectCallCount = savedRoundedRectCallCount;
  gRoundedRectBatchCallCount = savedRoundedRectBatchCallCount;
  gRoundedRectBatchItemCount = savedRoundedRectBatchItemCount;
  gTextDrawCallCount = savedTextDrawCallCount;
  gFpsTextDrawCallCount = savedFpsTextDrawCallCount;
  gCurrentFrameTextDrawCallCount = savedCurrentFrameTextDrawCallCount;
  gFpsTextChanges = savedFpsTextChanges;
  gLastFpsText = savedLastFpsText;

  int mismatches = 0;
  int firstX = -1;
  int firstY = -1;
  std::uint16_t firstPanel = 0;
  std::uint16_t firstFull = 0;
  for (int y = 0; y < 1440; y++) {
    for (int x = 0; x < 720; x++) {
      const std::size_t offset = static_cast<std::size_t>(y) * 720 + x;
      if (panel[offset] == full[offset]) continue;
      if (mismatches == 0) {
        firstX = x;
        firstY = y;
        firstPanel = panel[offset];
        firstFull = full[offset];
      }
      mismatches++;
    }
  }
  if (mismatches == 0) return true;
  std::fprintf(stderr,
               "[test_gea_bouncing_balls_jsx_main] presented panel differs from full replay after %s frame=%d: mismatches=%d first=(%d,%d) panel=0x%04x full=0x%04x\n",
               phase,
               frame,
               mismatches,
               firstX,
               firstY,
               firstPanel,
               firstFull);
  return false;
}

bool should_check_full_replay_reference(int frame) {
  return frame == 209 || frame == 210 || frame == 512 || frame == 1024;
}

void dump_node(int id, int indent = 0) {
  auto &tree = gea::embedded::ui::Tree::instance();
  if (id < 0 || id >= tree.nodeCount()) return;
  const auto &node = tree.node(id);
  const std::string pad(static_cast<std::size_t>(indent), ' ');
  std::fprintf(stderr,
               "%s#%d class=\"%s\" text=\"%s\" left=%d top=%d width=%d height=%d opacity=%u display=%d\n",
               pad.c_str(),
               id,
               tree.className(id).c_str(),
               node.text.c_str(),
               node.style.pos_offsets[3],
               node.style.pos_offsets[0],
               node.style.width,
               node.style.height,
               static_cast<unsigned>(node.style.opacity),
               static_cast<int>(node.style.display));
  for (int child = node.first_child; child >= 0; child = tree.node(child).next_sibling) dump_node(child, indent + 2);
}

void dump_tree() {
  auto &tree = gea::embedded::ui::Tree::instance();
  std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] nodeCount=%d mountedRoot=%d\n", tree.nodeCount(), tree.mountedRoot());
  dump_node(tree.mountedRoot());
}

void pump_frame(double timestamp_ms) {
  gea::framework::app::generated::drainMicrotasks();
  gea::host::runAnimationFrameCallbacks(timestamp_ms);
  gea::embedded::ui::Document::instance().frame(static_cast<int>(timestamp_ms));
  gea::embedded::ui::Document::instance().refreshMountedIfDirty();
}
}  // namespace

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

bool gea::platform::display::Display::init() { return true; }
bool gea::platform::display::Display::start() { return true; }
gea::framework::graphics::Canvas *gea::platform::display::Display::canvas() { return &gDisplayCanvas; }
void gea::platform::display::Display::clear() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::clearNoFlush() { gDisplayCanvas.clear(0); }
void gea::platform::display::Display::print(const char * /*text*/) {}
void gea::platform::display::Display::flush() {
  gFlushCallCount++;
  int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
  if (gDisplayCanvas.dirty(&bx0, &by0, &bx1, &by1)) {
    const long long px =
        static_cast<long long>(bx1 - bx0 + 1) * static_cast<long long>(by1 - by0 + 1);
    if (px > gMaxFlushDirtyBboxPixels) gMaxFlushDirtyBboxPixels = px;
  }
  copy_display_dirty_to_panel();
  gDisplayCanvas.resetDirty();
}
void recordTextDraw(const char *text);
bool gea::platform::display::Display::present(const DisplayPresentCommand *commands, int command_count) {
  if (!commands || command_count <= 0) return false;
  for (int i = 0; i < command_count; i++) {
    const DisplayPresentCommand &command = commands[i];
    switch (command.type) {
    case DisplayPresentCommandType::Clear:
      gDisplayCanvas.clear(command.clear.color);
      break;
    case DisplayPresentCommandType::FillCirclesRgb565:
      gFlushPixelCount += command.fillCirclesRgb565.count;
      gDisplayCanvas.setGlobalAlpha(command.fillCirclesRgb565.alpha);
      gDisplayCanvas.fillCirclesRgb565(command.fillCirclesRgb565.xs,
                                       command.fillCirclesRgb565.ys,
                                       command.fillCirclesRgb565.count,
                                       command.fillCirclesRgb565.radius,
                                       command.fillCirclesRgb565.colors);
      gDisplayCanvas.setGlobalAlpha(gDisplayAlpha);
      break;
    case DisplayPresentCommandType::FillText:
      recordTextDraw(command.fillText.text);
      gDisplayCanvas.setGlobalAlpha(command.fillText.alpha);
      gDisplayCanvas.drawText(command.fillText.text,
                              command.fillText.x,
                              command.fillText.y,
                              command.fillText.color,
                              command.fillText.scale);
      gDisplayCanvas.setGlobalAlpha(gDisplayAlpha);
      break;
    default:
      break;
    }
  }
  copy_display_to_panel_rect(0, 0, 719, 1439);
  gDisplayCanvas.resetDirty();
  flush();
  return true;
}
void gea::platform::display::Display::setFlushConfig(int /*chunk_rows*/, int /*queue_depth*/) {}
int gea::platform::display::Display::flushChunkRows() { return 0; }
int gea::platform::display::Display::flushQueueDepth() { return 0; }
int gea::platform::display::Display::flushBufferBytes() { return 0; }
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
void gea::platform::display::Display::fillRect(int x, int y, int w, int h, uint16_t color) {
  gFlushPixelCount += std::max(0, w) * std::max(0, h);
  gDisplayCanvas.fillRect(x, y, w, h, color);
}
void gea::platform::display::Display::scrollRect(int x, int y, int w, int h, int dx, int dy) { gDisplayCanvas.scrollRect(x, y, w, h, dx, dy); }
void gea::platform::display::Display::resetScrollRegion() { gDisplayCanvas.setScrollRegion(0, 0, 0); }
void gea::platform::display::Display::strokeRect(int x, int y, int w, int h, uint16_t color) { gDisplayCanvas.strokeRect(x, y, w, h, color); }
void gea::platform::display::Display::fillCircle(int cx, int cy, int r, uint16_t color) {
  gFlushPixelCount += std::max(0, r) * std::max(0, r);
  gDisplayCanvas.fillCircle(cx, cy, r, color);
}
void gea::platform::display::Display::strokeCircle(int cx, int cy, int r, uint16_t color) { gDisplayCanvas.strokeCircle(cx, cy, r, color); }
void gea::platform::display::Display::drawLine(int x0, int y0, int x1, int y1, uint16_t color) { gDisplayCanvas.drawLine(x0, y0, x1, y1, color); }
void gea::platform::display::Display::drawArc(int /*cx*/, int /*cy*/, int /*r*/, int /*start_deg*/, int /*end_deg*/, uint16_t /*color*/) {}
void gea::platform::display::Display::fillTriangle(int /*x0*/, int /*y0*/, int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, uint16_t /*color*/) {}
void recordTextDraw(const char *text) {
  gTextDrawCallCount++;
  gCurrentFrameTextDrawCallCount++;
  if (text && std::string(text).find("FPS: ") != std::string::npos) {
    gFpsTextDrawCallCount++;
    const std::string next(text);
    if (!gLastFpsText.empty() && gLastFpsText != next) gFpsTextChanges++;
    gLastFpsText = next;
  }
}

void gea::platform::display::Display::drawText(const char *text, int x, int y, uint16_t color, float scale) {
  recordTextDraw(text);
  gDisplayCanvas.drawText(text, x, y, color, scale);
}
void gea::platform::display::Display::drawTextFont(const char *text, int x, int y, uint16_t color, int font_id) {
  recordTextDraw(text);
  gDisplayCanvas.drawTextFont(text, x, y, color, font_id);
}
void gea::platform::display::Display::drawTextFontFamily(
    const char *text, int x, int y, uint16_t color, int family_id, int size_px) {
  recordTextDraw(text);
  gDisplayCanvas.drawTextFontFamily(text, x, y, color, family_id, size_px);
}
void gea::platform::display::Display::setPixel(int x, int y, uint16_t color) {
  gFlushPixelCount++;
  gDisplayCanvas.fillRect(x, y, 1, 1, color);
}
void gea::platform::display::Display::fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, uint16_t color) {
  gRoundedRectCallCount++;
  gFlushPixelCount += std::max(0, w) * std::max(0, h);
  gDisplayCanvas.fillRoundedRect(x, y, w, h, tl, tr, br, bl, color);
}
void gea::platform::display::Display::fillRoundedRectBoxesRgb565(
    const int16_t *xs, const int16_t *ys, int count,
    int w, int h, int tl, int tr, int br, int bl, const uint16_t *colors) {
  gRoundedRectBatchCallCount++;
  gRoundedRectBatchItemCount += std::max(0, count);
  gFlushPixelCount += std::max(0, count) * std::max(0, w) * std::max(0, h);
  gDisplayCanvas.fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
}
void gea::platform::display::Display::strokeRoundedRect(
    int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, uint16_t color) {
  gDisplayCanvas.strokeRoundedRect(x, y, w, h, tl, tr, br, bl, lw, color);
}
void gea::platform::display::Display::blitImage(const gea::framework::graphics::pixel::native_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy) {
  gDisplayCanvas.drawImage(src, alpha, src_w, src_h, dx, dy);
}
void gea::platform::display::Display::blitImageScaled(
    const uint16_t *src, const uint8_t *alpha, int src_w, int src_h, int dx, int dy, int dst_w, int dst_h) {
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
gea::platform::display::DisplayFlushStageDetail gea::platform::display::Display::flushStageDetail() { return {}; }
void gea::platform::display::Display::flushRects(const gea::platform::display::DisplayFlushRect *rects, int count, bool) {
  gFlushCallCount++;
  gFlushRectCallCount++;
  int pixels = 0;
  for (int i = 0; rects && i < count; i++) {
    const int x0 = std::max(0, rects[i].x0);
    const int y0 = std::max(0, rects[i].y0);
    const int x1 = std::min(719, rects[i].x1);
    const int y1 = std::min(1439, rects[i].y1);
    if (x0 <= x1 && y0 <= y1) pixels += (x1 - x0 + 1) * (y1 - y0 + 1);
    const int width = x0 <= x1 ? (x1 - x0 + 1) : 0;
    if (width > gMaxFlushRectWidth) gMaxFlushRectWidth = width;
    if (width > 0) gFlushRectItemCount++;
    if (width >= 720) {
      gFullWidthFlushRectItemCount++;
    } else if (width > gMaxPartialFlushRectWidth) {
      gMaxPartialFlushRectWidth = width;
    }
    copy_display_to_panel_rect(x0, y0, x1, y1);
  }
  gFlushRectPixelCount += pixels;
  if (pixels > gMaxFlushRectPixels) gMaxFlushRectPixels = pixels;
  gDisplayCanvas.resetDirty();
}
bool gea::platform::display::Display::streamRect(int, int, int, int, DisplayStreamRasterFn, void *) {
  return false;
}

namespace gea::host {
// Audio facade stubs: domNodeValue() (always compiled) builds an HTMLAudioElement
// from <audio> nodes, so these must resolve even though this app plays no audio.
struct HTMLAudioElement::State {
  int nodeId = -1;
};
HTMLAudioElement::HTMLAudioElement(const gea::embedded::ui::NodeHandle &node) : state_(std::make_shared<State>()) { state_->nodeId = node.id(); }
bool HTMLAudioElement::play() const { return true; }
void HTMLAudioElement::pause() const {}
}  // namespace gea::host

namespace gea::platform::audio {
// ui/tree_events.cpp's applyAudioAttribute() (always compiled) calls this
// directly. The real implementation (audio_runtime.cpp) is ESP-only.
bool AudioSystem::playFile(const std::string & /*path*/) { return false; }
}  // namespace gea::platform::audio

int main() {
  std::signal(SIGALRM, fail_on_alarm);
  alarm(5);

  gea::embedded::ui::Document::setPreferredMountSize(720, 1440);
  gea::embedded::ui::setViewportMetrics(720, 1440, 2.0);
  gDisplayPixels.assign(720 * 1440, 0);
  gPanelPixels.assign(720 * 1440, 0);
  gDisplayCanvas.bindPixels(gDisplayPixels.data(), 720, 1440);
  __gea_top_level();
  gea::framework::app::generated::drainMicrotasks();
  gea::embedded::ui::Document::instance().refreshMountedIfDirty();

  auto text = root_text_content();
  if (!expect_contains(text, "FPS: --", "initial text")) {
    dump_tree();
    return 1;
  }
  if (!expect_node_size("app", 720, 1440) || !expect_node_size("ball-field", 720, 1440)) {
    return 1;
  }

  const auto retained_balls = count_nodes_with_class("ball");
  if (retained_balls != kExpectedBallCount) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected %d retained JSX ball nodes, got %zu\n",
                 kExpectedBallCount,
                 retained_balls);
    dump_tree();
    return 1;
  }
  const int first_ball = first_node_with_class("ball");
  const auto &first_ball_node = gea::embedded::ui::Tree::instance().node(first_ball);
  if (first_ball_node.layout.width != 16 || first_ball_node.layout.height != 16 ||
      first_ball_node.layout.x < 8 || first_ball_node.layout.x > 704 ||
      first_ball_node.layout.y < 8 || first_ball_node.layout.y > 1424) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected DPR 2 retained balls to keep 16px physical layout inside viewport, got (%d,%d %dx%d)\n",
                 first_ball_node.layout.x,
                 first_ball_node.layout.y,
                 first_ball_node.layout.width,
                 first_ball_node.layout.height);
    dump_tree();
    return 1;
  }
  int maxInitialBallX = 0;
  int maxInitialBallY = 0;
  auto &initial_tree = gea::embedded::ui::Tree::instance();
  for (int id = 0; id < initial_tree.nodeCount(); id++) {
    if (!initial_tree.hasClass(id, "ball")) continue;
    const auto &node = initial_tree.node(id);
    maxInitialBallX = std::max(maxInitialBallX, static_cast<int>(node.layout.x));
    maxInitialBallY = std::max(maxInitialBallY, static_cast<int>(node.layout.y));
  }
  if (maxInitialBallX <= 500 || maxInitialBallY <= 900) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected responsive ball seeding across 720x1440 viewport, max=(%d,%d)\n",
                 maxInitialBallX,
                 maxInitialBallY);
    dump_tree();
    return 1;
  }
  const int fps_badge = first_node_with_class("fps-badge");
  if (fps_badge < 0) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected retained JSX FPS badge node\n");
    dump_tree();
    return 1;
  }
  const auto &fps_badge_node = gea::embedded::ui::Tree::instance().node(fps_badge);
  if (fps_badge_node.style.font_size != kExpectedFpsFontSize || fps_badge_node.style.font_id < 0) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected FPS badge to keep generated Inter %dpx font, got size=%d font_id=%d\n",
                 kExpectedFpsFontSize,
                 fps_badge_node.style.font_size,
                 fps_badge_node.style.font_id);
    dump_tree();
    return 1;
  }
  gea::embedded::ui::DisplayList::instance().maybeBakeStaticBackdrop(720, 1440, true, true);
  gea::embedded::ui::DisplayList::instance().maybeBakeStaticBackdrop(720, 1440, true, true);
  if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] simple retained JSX balls must not arm the static backdrop cache\n");
    return 1;
  }
  gea::embedded::ui::Tree::instance().setText(fps_badge, "FPS: 777");
  pump_frame(21.0 * 16.0);

  gFlushCallCount = 0;
  gFlushPixelCount = 0;
  gRoundedRectCallCount = 0;
  gRoundedRectBatchCallCount = 0;
  gRoundedRectBatchItemCount = 0;
  gFlushRectCallCount = 0;
  gFlushRectItemCount = 0;
  gFullWidthFlushRectItemCount = 0;
  gFlushRectPixelCount = 0;
  gMaxFlushRectPixels = 0;
  gMaxFlushRectWidth = 0;
  gMaxPartialFlushRectWidth = 0;
  gMaxFlushDirtyBboxPixels = 0;
  gTextDrawCallCount = 0;
  gFpsTextDrawCallCount = 0;
  gCurrentFrameTextDrawCallCount = 0;
  gFramesWithFpsTextReplay = 0;
  gFpsTextChanges = 0;
  gLastFpsText.clear();
  text = root_text_content();
  std::string previousFrameText = text;
  bool sawFpsTextChange = false;
  int rootTextChanges = 0;
  for (int frame = 22; frame <= 1521; frame++) {
    gCurrentFrameTextDrawCallCount = 0;
    pump_frame(static_cast<double>(frame * 16));
    if (should_check_full_replay_reference(frame) &&
        !expect_incremental_frame_matches_full_replay("moving ball", frame))
      return 1;
    if (should_check_full_replay_reference(frame) &&
        !expect_presented_panel_matches_full_replay("moving ball", frame))
      return 1;
    if (gea::embedded::ui::DisplayList::instance().staticBackdropActive()) {
      std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] simple retained JSX animation must not bake a full-surface backdrop cache\n");
      return 1;
    }
    if (gCurrentFrameTextDrawCallCount > 0) gFramesWithFpsTextReplay++;
    const std::string currentFrameText = root_text_content();
    if (currentFrameText != previousFrameText) {
      sawFpsTextChange = true;
      rootTextChanges++;
      previousFrameText = currentFrameText;
    }
  }
  const auto final_balls = count_nodes_with_class("ball");
  if (final_balls != kExpectedBallCount) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected animation to preserve %d retained JSX ball nodes, got %zu\n",
                 kExpectedBallCount,
                 final_balls);
    dump_tree();
    return 1;
  }
  if (!sawFpsTextChange) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected FPS text to change during animation window\n");
    return 1;
  }
  if (!gea::embedded::ui::DisplayList::instance().canReplaySimpleDirtyRegions(720, 1440)) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected bouncing-balls retained tree to stay on the simple dirty replay path\n");
    return 1;
  }
  auto &tree = gea::embedded::ui::Tree::instance();
  const auto &fps_after_animation = tree.node(fps_badge);
  const int fps_x0 = fps_after_animation.layout.x;
  const int fps_y0 = fps_after_animation.layout.y;
  const int fps_x1 = fps_x0 + fps_after_animation.layout.width - 1;
  const int fps_y1 = fps_y0 + fps_after_animation.layout.height - 1;
  const int beforeReplayBrightPixels = bright_pixels_in_rect(fps_x0, fps_y0, fps_x1, fps_y1);
  if (beforeReplayBrightPixels <= 0) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected rendered Inter FPS glyph pixels before dirty-region replay, got 0 in (%d,%d)-(%d,%d)\n",
                 fps_x0,
                 fps_y0,
                 fps_x1,
                 fps_y1);
    return 1;
  }
  if (!write_display_ppm(std::getenv("GEA_BOUNCING_BALLS_JSX_DUMP_PPM"))) return 1;
  gDisplayCanvas.fillRect(fps_x0, fps_y0, fps_after_animation.layout.width, fps_after_animation.layout.height, 0);
  gDisplayCanvas.resetDirty();
  gea::platform::display::Display::resetClip();
  gea::platform::display::Display::setAlpha(255);
  gea::platform::display::Display::pushClip(fps_x0, fps_y0, fps_after_animation.layout.width, fps_after_animation.layout.height);
  gea::embedded::ui::DisplayList::instance().replaySimpleClippedDirtyRegion(fps_x0, fps_y0, fps_x1, fps_y1);
  gea::platform::display::Display::popClip();
  const int afterReplayBrightPixels = bright_pixels_in_rect(fps_x0, fps_y0, fps_x1, fps_y1);
  if (afterReplayBrightPixels < beforeReplayBrightPixels / 2) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected retained dirty-region replay to restore FPS label pixels, before=%d after=%d textFrames=%d fpsChanges=%d fpsTextDraws=%d totalTextDraws=%d\n",
                 beforeReplayBrightPixels,
                 afterReplayBrightPixels,
                 gFramesWithFpsTextReplay,
                 gFpsTextChanges,
                 gFpsTextDrawCallCount,
                 gTextDrawCallCount);
    return 1;
  }
  if (gFlushCallCount == 0 && gFlushRectCallCount == 0) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected retained moving-ball frames to flush dirty regions\n");
    return 1;
  }
  if (gRoundedRectBatchCallCount == 0 || gRoundedRectBatchItemCount < gRoundedRectCallCount) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] expected retained ball replay to use rounded-rect batching, batchCalls=%d batchItems=%d scalarCalls=%d\n",
                 gRoundedRectBatchCallCount,
                 gRoundedRectBatchItemCount,
                 gRoundedRectCallCount);
    return 1;
  }
  // Regression guard for the afbf1c7b union-flush change: balls scattered across
  // the panel must be flushed as sparse per-rect windows, never coalesced into one
  // full-surface union flush. Their dirty rects cover only a few percent of the
  // surface, but their bounding box spans almost all of it; a union flush there
  // copies+TXes ~10x the pixels and dropped hardware fps 62 -> 38. A healthy sparse
  // flush keeps each flush bbox tiny; budget 1/3 of the surface cleanly separates
  // the two (sparse is well under, the union regression is ~90%).
  const long long kFlushBboxBudget = static_cast<long long>(720) * 1440 / 3;
  // Scattered balls flush via Display::flushRects (sparse, batched); a regression to
  // a full-surface union shows up as either a giant single flush() bbox or a giant
  // single flushRects() call. Guard the larger of the two.
  const long long maxSingleFlushPixels =
      std::max<long long>(gMaxFlushDirtyBboxPixels, gMaxFlushRectPixels);
  if (maxSingleFlushPixels > kFlushBboxBudget) {
    std::fprintf(stderr,
                 "[test_gea_bouncing_balls_jsx_main] scattered ball dirty regions must flush sparsely, not as a full-surface union: maxSingleFlush=%lld px exceeds budget %lld px (union-flush regression)\n",
                 maxSingleFlushPixels,
                 kFlushBboxBudget);
    return 1;
  }

  text = root_text_content();
  if (!expect_contains(text, "FPS: ", "frame text")) {
    dump_tree();
    return 1;
  }
  if (text.find("FPS: --") != std::string::npos) {
    std::fprintf(stderr, "[test_gea_bouncing_balls_jsx_main] expected FPS readout to update after frames, got:\n%s\n", text.c_str());
    dump_tree();
    return 1;
  }
  if (std::getenv("GEA_BB_DUMP_COUNTERS")) {
    std::fprintf(stderr,
        "[BBCOUNTERS] frames=1500 framesWithFpsTextReplay=%d fpsTextDraws=%d fpsTextChanges=%d totalTextDraws=%d "
        "maxFlushRectWidth=%d maxPartialFlushRectWidth=%d fullWidthFlushItems=%d flushRectItems=%d "
        "maxFlushRectPixels=%lld surfaceW=720\n",
        gFramesWithFpsTextReplay, gFpsTextDrawCallCount, gFpsTextChanges, gTextDrawCallCount,
        gMaxFlushRectWidth, gMaxPartialFlushRectWidth, gFullWidthFlushRectItemCount, gFlushRectItemCount,
        static_cast<long long>(gMaxFlushRectPixels));
  }
  alarm(0);
  return 0;
}
