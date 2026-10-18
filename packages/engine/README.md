# @geastack/engine

The GeaStack rendering engine: the intrinsic JSX primitives an app builds its UI
from, and the C++ that lays them out, styles them and paints them on a native
target. It covers the document tree and layout, style resolution and CSS
animation, text and image rendering, canvas 2D, scrolling, the virtual keyboard
and input routing.

```sh
npm install @geastack/engine
```

```tsx
import { View, Text, Image, Button } from '@geastack/engine'

export default () => (
  <View style={{ padding: 16 }}>
    <Text>Hello</Text>
  </View>
)
```

## Checkbox

`Checkbox` accepts `checked`, `indeterminate`, `disabled`, `label`, `class`,
and `style` props.

`onChange(checked)` receives the next checked value when an enabled checkbox
is toggled. `onPress(event)` and `onClick(event)` run from the native `click`
path. `onPress` is not a native `press` event binding. A Space keydown toggles
the checkbox but does not invoke either callback.

The component prop types live in `@geastack/core`. The C++ sources are
compiled into a target through `@geastack/core`'s source manifest. Vendored
third-party code is listed in the repository's `THIRD_PARTY.md`.

## Renderer cache reachability

ESP32 app builds derive renderer cache storage from the Gea compiler plugin's
source analysis. Circle operations, CSS transforms, linear gradients and radial
gradients each retain their own caches only when used. A video canvas that calls
`clearRect` and `drawImage`, for example, does not reserve circle tables. Ordinary
rounded corners continue to render without them.

The analysis follows source imports, imported CSS and CSS `@import`, JSX style
objects, style assignments and DOM style setters. Unknown styles or opaque
imports conservatively retain the corresponding features. The CLI passes the
result to both the app and the shared engine; no per-app switches are needed.
Position storage is selected independently for each physical edge and for
pixel versus percentage offsets. Unknown/logical insets retain every possible
edge. Native inputs and lists retain the edges they can mutate. The selected
fields remain embedded, aligned members; selection happens at compile time,
with no runtime accessor or extra pointer load. Cached style operations also
omit transform payloads when transforms are absent.

Text and border alpha bytes are omitted only when the complete source graph
proves every reachable color opaque. Literal CSS, custom-color definitions,
immutable local bindings and bounded palette/getter values participate in that
build-time proof. Unknown receivers, writes, escaping objects, palette mutation,
reflection, and native controls retain storage. Background alpha is independent.
The retained values stay inline; omitted fields read as the constant 255.
Node records reuse tail padding while preserving member assignment and reset
semantics, without a pointer lookup or packed-field decoding.

CSS animation support is likewise derived from whole-source reachability.
Animation/transition declarations, keyframes, `data-anim`, imperative animation
APIs and opaque styles/native UI calls retain it. Animation-free apps omit the
compiled animation handle in each CSS rule, active-list storage, priming and
per-frame animation polling. `requestAnimationFrame` is independent and remains
available. Older analysis versions retain the full animation engine; application
defines cannot override the proof. Enabled animation and frame-scheduling tests
run alongside the pruned geometry/pixel checks.

Generated `::before`/`::after` node support is also automatic. Literal selectors,
`content` writes and opaque CSS/native mutation retain it; older analyzers keep
support by default. First-line and animation buckets remain independent. Absent
families reserve no rule-plan buckets or cached bucket counters. CSS resets copy
an immutable default style directly, without constructing a temporary Node or
initializing unrelated text, links, layout and render state.

Cache-size definitions affect only retained features. Older analyzers and native
builds without feature analysis keep the full engine defaults.

Pruned caches have no persistent storage. The engine retains uncached rendering
paths for native or dynamically supplied instructions, preserving pixels and hit
testing even when such instructions are absent from the source analysis.
`packages/core/test/run-renderer-features.sh` compares enabled/pruned rendering
and verifies that pruned cache storage symbols are absent from the binary.

## Native JPEG decoding

ESP targets that link Espressif's `esp_new_jpeg` component automatically use it
when its `esp_jpeg_dec.h` header is visible and the target stores RGB565 pixels.
The decoder writes directly to the aligned image buffer in the target's byte
order, preserving source dimensions. It does not allocate an intermediate RGBA
frame. Unsupported images fall back to the ESP32-S3 ROM decoder, then stb.
Targets without the component retain their existing decoder fallback.

The S3 ROM path also preserves full colour and source dimensions, converting
individual RGB888 MCU blocks into the final native image with an 8 KiB workspace.
`packages/core/test/run-native-jpeg.sh` checks the backend API contracts, native
pixels, byte order, cleanup and fallback using host mocks. Actual codec speed
and output must be checked on hardware. With `GEA_EMBEDDED_PERF=1`, the optional
Espressif backend reports its result and duration on the first and every tenth
decode.

## License

Apache-2.0. See `LICENSE`.
