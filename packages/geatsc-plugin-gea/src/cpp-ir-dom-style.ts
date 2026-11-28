// The typed style entry points live in gea/embedded.h and serve both code
// generation paths. This overload adapts the legacy IR's dynamic node value.
export function domStyleAccessorInteropSource(): string[] {
  return [
    'inline gea::embedded::ui::Style domStyle(const gea_cpp_value &node) {',
    '  return domStyle(gea::embedded::ui::NodeHandle(nodeIdFromValue(node)));',
    '}',
    ''
  ]
}
