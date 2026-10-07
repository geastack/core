#include "display.h"
#include "display_present.h"
#include "host/image.h"
#include "image.h"
#include "native_test_harness.h"
#include "platform/file_cache.h"
#include "ui/canvas_element.h"
#include "ui/document.h"
#include "ui/image.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// This fixture mounts runtime files and has no build-embedded asset table.
// Production builds provide this function through generated assets.
extern "C" __attribute__((weak)) bool
gea_embedded_asset_lookup(const char *, const unsigned char **, unsigned long *) {
  return false;
}

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void __attribute__((weak)) ensureLinked() {}
} // namespace gea::framework::graphics::generated

using namespace gea::embedded::ui;
using namespace gea::embedded::test;
namespace graphics = gea::framework::graphics;
namespace pixel = graphics::pixel;

static bool mounted() { return true; }

static int liveImages() {
  int live = 0;
  for (int id = 0; id < graphics::kImageMax; ++id)
    live += graphics::ImageStore::instance().width(id) > 0;
  return live;
}

static void writeGrayImage(const std::string &path, unsigned char gray) {
  std::vector<std::uint8_t> bytes{'G', 'T', 'H', 'M', 8, 0, 8, 0};
  bytes.resize(8 + 64, gray);
  assert(gea::host::image.writeFile(path, bytes));
}

static void assertGray(int x, int y, unsigned char gray) {
  if (displayPixelAt(x, y) != pixel::fromRgb888(gray, gray, gray))
    std::fprintf(stderr,
                 "FAIL pixel (%d,%d): actual=%x expected=%x gray=%u live=%d\n",
                 x, y, static_cast<unsigned>(displayPixelAt(x, y)),
                 static_cast<unsigned>(pixel::fromRgb888(gray, gray, gray)),
                 gray, liveImages());
  assert(displayPixelAt(x, y) == pixel::fromRgb888(gray, gray, gray));
}

static void assertSnapshotGray(int x, int y, unsigned char gray) {
  std::array<std::uint16_t, 32 * 32> snapshot{};
  assert(renderRetainedSnapshotRgb565(snapshot.data(), 32, 32));
  assert(snapshot[y * 32 + x] ==
         pixel::toRgb565(pixel::fromRgb888(gray, gray, gray)));
  // The null host does not implement snapshot canvas rebind restoration.
  // Restore its display binding before any later frame uses the stack buffer.
  setNativeDisplaySize(32, 32);
  DisplayList::instance().replay();
}

int main(int argc, char **argv) {
  assert(argc == 3);
  const std::string firstPath =
      std::string(argv[1]) + "/image-lifetime-first.gthm";
  const std::string secondPath =
      std::string(argv[1]) + "/image-lifetime-second.gthm";
  gea::platform::storage::setMountProvider(mounted);
  writeGrayImage(firstPath, 48);
  writeGrayImage(secondPath, 208);
  resetNativeHost();
  setNativeDisplaySize(32, 32);
  setViewportMetrics(32, 32, 1);
  auto &document = Document::instance();
  auto &images = graphics::ImageStore::instance();
  auto root = document.createView();
  root.style().width(32);
  root.style().height(32);
  root.style().backgroundColor(pixel::fromRgb888(0, 0, 0));
  auto image = document.createImage();
  image.style().width(8);
  image.style().height(8);
  root.appendChild(image);
  document.mount(root, 32, 32);

  // Exceed the entire 96-slot pool twice. Both pictures must still render,
  // with only the current decode and any retired frame reader alive.
  for (int step = 0; step < 200; ++step) {
    const bool second = step % 2;
    image.setAttribute("src", second ? secondPath.c_str() : firstPath.c_str());
    document.refresh(root, 32, 32);
    assertGray(2, 2, second ? 208 : 48);
    assertSnapshotGray(2, 2, second ? 208 : 48);
    assert(liveImages() <= 2);
  }

  // An overwritten upload at the same pathname must not show a cached decode.
  writeGrayImage(secondPath, 112);
  image.setAttribute("src", secondPath.c_str());
  document.refresh(root, 32, 32);
  assertGray(2, 2, 112);
  image.classList().set("image-lifetime-recompute");
  document.refresh(root, 32, 32);
  assertGray(2, 2, 112);

  // A detached clone shares its decode, then remains valid after the original
  // changes and is destroyed. Retained replay must not read reclaimed pixels.
  auto clone = image.cloneNode(false);
  clone.style().setProperty("position", "absolute");
  clone.style().left(16);
  clone.style().top(0);
  root.appendChild(clone);
  image.setAttribute("src", firstPath.c_str());
  // The previous frame owns its original pixels until rerecording completes.
  assertSnapshotGray(2, 2, 112);
  document.refresh(root, 32, 32);
  assertGray(2, 2, 48);
  assertGray(18, 2, 112);
  image.remove();
  document.refresh(root, 32, 32);
  assertSnapshotGray(18, 2, 112);
  clone.remove();
  document.refresh(root, 32, 32);
  DisplayList::instance().clear();
  assert(liveImages() == 0);

  // Disposing a handle after enqueueing a canvas image must not invalidate
  // the batch's raw source pointer before endBatch consumes it.
  auto canvas = document.createCanvas();
  canvas.style().width(8);
  canvas.style().height(8);
  root.appendChild(canvas);
  document.refresh(root, 32, 32);
  auto context = CanvasElement(canvas.id()).getContext2D();
  const int canvasId = static_cast<int>(gea::host::image.loadFile(firstPath));
  assert(canvasId >= 0);
  gea::platform::display::DisplayPresentCommand exported{};
  exported.type = gea::platform::display::DisplayPresentCommandType::DrawImage;
  exported.drawImage = {images.currentPixels(canvasId),
                        images.currentAlpha(canvasId),
                        8,
                        8,
                        0,
                        0,
                        255};
  gea::framework::display_present::Frame extracted;
  assert(
      gea::framework::display_present::extractFrame(&exported, 1, extracted));
  context.beginBatch();
  context.drawImage(canvasId, 0, 0);
  images.dispose(canvasId);
  context.endBatch();
  document.refresh(root, 32, 32);
  assertGray(2, 2, 48);
  assertSnapshotGray(2, 2, 48);
  canvas.remove();
  document.refresh(root, 32, 32);
  DisplayList::instance().clear();
  assert(liveImages() == 1);
  std::array<pixel::native_t, 64> retiredFrame{};
  gea::framework::display_present::rasterFrameRows(retiredFrame.data(), 0, 8, 0,
                                                   8, 8, extracted);
  for (const auto color : retiredFrame)
    assert(color == pixel::fromRgb888(48, 48, 48));
  extracted.commands.clear();
  assert(liveImages() == 0);

  // Public handles keep their existing explicit-dispose contract, but an
  // already recorded frame must survive disposal until that reader retires.
  auto explicitImage = document.createImage();
  explicitImage.style().width(8);
  explicitImage.style().height(8);
  root.appendChild(explicitImage);
  const int explicitId = static_cast<int>(gea::host::image.loadFile(firstPath));
  assert(explicitId >= 0);
  ImageElement(explicitImage.id()).imageId(explicitId);
  document.refresh(root, 32, 32);
  images.dispose(explicitId);
  assertSnapshotGray(2, 2, 48);
  explicitImage.remove();
  document.refresh(root, 32, 32);
  DisplayList::instance().clear();
  assert(liveImages() == 0);

  // Real JPEG loading follows the same filesystem ownership path.
  auto jpeg = document.createImage();
  jpeg.style().width(8);
  jpeg.style().height(8);
  root.appendChild(jpeg);
  for (int step = 0; step < 100; ++step) {
    jpeg.setAttribute("src", argv[2]);
    document.refresh(root, 32, 32);
    assert(liveImages() <= 2);
  }
  jpeg.removeAttribute("src");
  document.refresh(root, 32, 32);
  DisplayList::instance().clear();
  assert(liveImages() == 0);
  jpeg.setAttribute("src", argv[2]);
  document.refresh(root, 32, 32);
  assert(liveImages() == 1);
  document.clear();
  DisplayList::instance().clear();
  assert(liveImages() == 0);
  // Reusing a frame for a shorter non-image frame, or an empty frame, must
  // release its previous image leases even when vector capacity is retained.
  for (int replacement = 0; replacement < 2; ++replacement) {
    const int id = static_cast<int>(gea::host::image.loadFile(firstPath));
    exported.drawImage.pixels = images.currentPixels(id);
    assert(
        gea::framework::display_present::extractFrame(&exported, 1, extracted));
    images.dispose(id);
    assert(liveImages() == 1);
    if (replacement == 0) {
      gea::platform::display::DisplayPresentCommand empty{};
      empty.type = gea::platform::display::DisplayPresentCommandType::Clear;
      empty.clear.color = 0;
      assert(
          gea::framework::display_present::extractFrame(&empty, 1, extracted));
    } else {
      assert(!gea::framework::display_present::extractFrame(nullptr, 0,
                                                            extracted));
    }
    assert(liveImages() == 0);
  }
  std::puts("PASS image lifetime: 200 toggles, overwrite, clone/remove, "
            "retained disposal, canvas batch, 100 JPEG reloads");
}
