// The display repaint hook's strong definition for every native test build,
// with or without the test host (GEA_NATIVE_TEST_INCLUDE_HOST).
//
// engine/ui/tree_render.cpp and engine/ui/root_scroll_refresh.cpp each define
// a weak no-op default. ELF and Mach-O keep one of them, but on Windows (COFF)
// lld-link takes the second unit's weak definition for a duplicate symbol
// unless a strong one has been seen first (native-test-common.sh links this
// unit first). The test builds have no display to prepare, so this no-op
// changes nothing on other hosts.
namespace gea::platform::display {
struct DisplayFlushRect;
}

extern "C" void gea_display_prepare_repaint(const gea::platform::display::DisplayFlushRect * /*rects*/, int /*count*/) {}
