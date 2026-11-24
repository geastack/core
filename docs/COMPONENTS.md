# Component Contribution Contract

This document defines what a finished Gea UI component contribution looks like.
It applies to the components tracked on the
[Gea UI Elements board](https://github.com/orgs/geastack/projects/2) and to any
new non-intrinsic element added to `@geastack/elements`.

The rules are drawn from the two components that already ship in
`@geastack/elements` (`VirtualList` and `CameraView`). Where this document makes
a choice those components do not settle, it says so.

## Reuse First

These facilities already exist. A contribution builds on them and does not
rebuild them:

| Need | Existing facility |
|---|---|
| Layout boxes, text | `div`, `span`, `p`, `label` (`engine/components` `View`, `Text`) |
| Buttons | `button` (`engine/components` `Button`) |
| Text entry | `input`, `textarea`, the virtual keyboard |
| Scrolling | `overflow: scroll` containers |
| Long lists | `VirtualList` (`@geastack/elements`) |
| Custom drawing | `canvas` |
| Images, GIFs | `img` |
| Camera preview | `CameraView` (`@geastack/elements`) |

A PR that adds a new native tag or backend explains which of these was tried and
why it was not enough.

## Package Placement

- Components live in `packages/elements/components/<Name>.tsx`.
  `packages/elements` holds the non-intrinsic JSX elements;
  `packages/engine` holds the intrinsic primitives.
- Do not add a wrapper to `packages/engine/components/`. Those files are 1:1
  aliases of intrinsic tags. A new wrapper is only justified when the PR also
  adds a new intrinsic tag.
- A native backend, when one is needed, goes in
  `packages/elements/ui/<name>.cpp` and is listed in
  `packages/core/gea_sources.mjs` next to `camera_element` and `virtual_list`.
  The backend alone does not make a new tag. `virtual-list` and `camera` also
  need a `NodeType` (`engine/ui/node_model.h`) and its handling across
  `engine/ui`, a `Document` factory (`createVirtualList()`, `createCamera()`),
  an entry in both geatsc tag maps (`geatsc-plugin-gea/src/cpp-mounted.ts` and
  `cpp-template-renderer.ts`), and a typing in `GeaIntrinsicElements`
  (`packages/core/index.d.ts`).

## Files

A component named `Name` touches these files:

| File | Content |
|---|---|
| `components/Name.tsx` | The component and its exported `NameProps` interface. |
| `components/Name.css` | The default stylesheet, imported from `Name.tsx` with `import './Name.css'`. |
| `components/Name.d.ts` | The public declaration: the same props interface and `export default function Name(props: NameProps): any` (or `export declare class` for a class component). |
| `components/index.ts` | `export { default as Name } from './Name'` for a function component, `export { Name } from './Name'` for a class component (as `CameraView` does), plus `export type { NameProps } from './Name'`. |
| `components/index.d.ts` | The same exports. |
| `package.json` | An `exports` entry `"./components/Name"` with `types` and `default`. |
| `README.md` | One bullet describing the component. |

Keep `Name.tsx` and `Name.d.ts` in sync by hand; there is no generator.

Export the types from `index.ts` too, not only from `index.d.ts`. A native
build resolves value imports of `@geastack/elements` to `index.ts`, so
`import { Name, type NameProps } from '@geastack/elements'` passes `tsc` but
fails in geatsc with "has no exported member" when `index.ts` lacks the type.

## Component Shape

**Function component by default.** `VirtualList` is a plain function of its
props. Use a function unless the component owns something that must be released.

**Class component only for owned resources.** `CameraView` extends `Component`
because it opens the camera. A class component:

- states its props as the second type argument and destructures them in
  `template`: `class Name extends Component<GeaElement, NameProps>` with
  `template({ value, onChange }: NameProps)`, the form
  `examples/apps/e-reader/components/BookCover.tsx` uses. The base declaration
  in `packages/core/index.d.ts` is `template(props?: Props)`; copying that
  optional parameter would force `props?.x` on every access under `strict`.
  `CameraView` takes its props in the constructor instead; no app exercises
  that form, so do not copy it;
- exposes a public method that releases what it owns (`CameraView.close()`);
- documents in the `.d.ts` and README that the app must call that method,
  because `Component` has no unmount hook and nothing calls it automatically.

**Controlled state.** The app owns the value; the component renders it and
reports changes. Keep values in the app or a `Store`, not in module-level
variables or hidden component fields. `VirtualList` is only partly a precedent:
it takes `scrollTop` from the app, but the native element owns the scroll
offset and the app copies it back from `onScroll`.

## Props And Events

Reuse the names `@geastack/core` already uses for native controls
(`InputElementProps`, `NativeSelectProps`, `NativeOptionProps`,
`NativeStepperProps`, `NativeProgressProps` in `packages/core/index.d.ts`):

| Purpose | Name |
|---|---|
| Current value | `value` |
| Boolean state | `checked`, `selected`, `disabled`, `readOnly`, `indeterminate` |
| Numeric range | `min`, `max`, `step` |
| Styling | `class` (`ClassValue`) |
| Identity | `id` |
| Press | `onClick` |

`onFocus` and `onBlur` are reserved for core#5. geatsc lowers them to `focus`
and `blur` listeners, but the engine has no slot for those event types and
drops them, so they fire on no native target today. Do not take them as props
yet. The engine does dispatch bubbling `focusin` and `focusout` events when the
active input changes (`Tree::setActiveInput` in `engine/ui/input.cpp`): when
the user taps a text input, when a tap lands outside every input (focus is
cleared, so an element focused with `focus()` loses it on the next such tap),
or when the app calls `element.focus()` on a connected element that is not
disabled. On the pixel engine any such element takes focus; a native renderer
can refuse it (`requestNativeFocus`, for example on macOS when the view does
not accept first responder).

Do not take a `style` prop in a function component. Building the ProgressBar
component, a `style?: Style` prop forwarded to a JSX `style` attribute passed
geatsc but failed to compile as C++: a `static_assert` in the compiler's
`gea_runtime.h` reports "no attribute/style/listener rule installed for this
prop value's C++ type". Put the caller's `class` on the root and let the app
size and place the component through it. The `engine/components` wrappers
(`View`, `Text`, `Button`, `Image`) forward `style` the same way, and no app
in geastack/examples compiles them, so do not take them as evidence that it
works.

**Value changes are reported as `onChange(next: T)`**, where `T` is the
component's value type (`boolean` for a switch, `number` for a slider, the item
type for a select). The component calls it with the proposed next value and does
not apply it itself; the app decides.

> This is the one rule with no precedent in the existing elements. Native
> controls report `onInput(event: InputEvent)` for each edit and
> `onChange(event: InputEvent)` for a committed change, both carrying a string
> `currentTarget.value`. A typed callback avoids parsing strings back into
> numbers and booleans in every app, and matches `CameraView`'s typed
> `onReady`/`onError`. It does give `onChange` a different payload on a
> component than on a native element. Maintainers may prefer the native
> `InputEvent` form, or another name for the typed callback; settle it in
> review before many components land.

Other event props are `on` + a verb with a typed payload (`onReady(cam)`,
`onError(message)`), never an untyped `any`.

## Styling And State

The native style engine supports type, class, id, `*`, descendant and `>`
selectors, and only the `:root`, `:first-child`, `:last-child` and `:hover`
pseudo-classes, plus `.name:active` on a single class for press feedback (its
`background-color` only; the build turns it into `active-background-color`).
There is no `:focus`, `:checked` or `:disabled`, no attribute
selector, and no `+` or `~` sibling combinator. The DOM queries
(`querySelector`, `matches`, `closest`) use a separate parser
(`engine/ui/selector.cpp`) that does accept attribute selectors and
`:disabled`; stylesheet rules do not.

Express state with class maps instead:

```tsx
<div class={{ switch: true, 'switch--on': checked, 'switch--disabled': disabled }} />
```

Rules:

- Ship a default stylesheet with stable, documented class names, and accept
  `class` so the app can override them.
- Put the caller's `class` alone on the root element and give the inner parts
  fixed `name__part` classes for the default stylesheet. `ClassValue` has no
  array form, so the root cannot carry both the caller's value and a default
  class without the component merging them by hand.
- Do not encode state in attributes and select on them; it will not match.
- Theme tokens (core#3) and focus styling (core#5) are not settled yet. Until
  they are, use plain colors in the default stylesheet and keep them in one
  place so they can move to tokens later.

## Native Representation

A component renders through one of two paths. Pick one and state it in the PR.

**Composed.** The component is built from `div`, `span`, `canvas` and the other
generic primitives. Every target that renders those primitives renders the
component. This is the default.

**Native control tag.** The component renders an existing control tag:
`input type="checkbox" | "range" | "radio"`, `select`, `stepper` or
`progress`. These tags are typed in `GeaIntrinsicElements`, but targets render
them differently:

| Tag | Pixel engine (ESP32, RP2350, WASM simulator, Linux, Android) | macOS | iOS | Windows |
|---|---|---|---|---|
| `input type="checkbox"` | text-field box (`engine/ui/input_render.cpp`) | `NSSwitch`; a checkbox with `data-control="checkbox"` | native text field | check box |
| `input type="radio"` | text-field box | radio `NSButton` | native text field | text field |
| `input type="range"` | text-field box | `NSSlider` | native text field | trackbar |
| `select` | plain view | `NSPopUpButton` | plain view | plain view |
| `stepper` | plain view | `NSStepper` | plain view | plain view |
| `progress` | plain view | `NSProgressIndicator` | plain view | progress bar |

The Linux and Android targets draw through the same pixel engine into a
framebuffer (`rpios_display.cpp`, `android_display.cpp`). iOS mirrors each node
as a UIKit view (`syncNativeTree` in `renderer/view_reconciler.mm`): labels,
buttons, images, canvases and scroll containers, a native text field for every
`input`, and a plain `UIView` for `select`, `stepper` and `progress`.

So a component that renders a native control tag looks right only on macOS for
all six tags, and on Windows only for checkbox, range and progress. It looks
wrong everywhere else unless the pixel engine learns to draw that tag. A PR
that takes this path either adds that rendering to `packages/engine` or limits
its stated target scope and says so.

The browser target (`gea dev`, and `gea simulate`, whose default renderer is
now the DOM) renders these tags as browser controls and applies full browser
CSS. A component that looks right there has not been shown to work on any
native target; use `gea simulate --renderer wasm` or the native pipeline test
for that. Pebble has its own small flexbox engine that paints with PebbleOS
graphics and system fonts; this document describes the core engine and does
not cover Pebble.

A new intrinsic tag also needs a mapping in every renderer that creates native
views: the macOS, iOS and Windows renderers. Check the debugger's device
preview (`debugger/src/device-preview.mjs`, which special-cases `canvas`) and
update the tag lists in the `skills` repo.

## Compiler Constraints

Component code is compiled ahead of time to C++ by `geatsc`. Write code that
compiles under the compiler's static representation policy, without
`--dynamic-fallback` (see `docs/DYNAMIC-FALLBACK.md` in the compiler repo):

- Give every prop and local a static type. Values that need runtime dispatch
  are boxed only under `--dynamic-fallback`, which apps cannot rely on.
- Pass `class` through as a `ClassValue` (`string | ClassMap`); the JSX `class`
  prop already accepts both, so do not normalize it by hand.

The behavior test below builds the example app through the same pipeline as
the device targets. Like `run-gea-virtual-list-pipeline.sh`, it passes
`--allow-any`, so it shows the component builds in that configuration, not
that it builds without the flag.

## Text Rendering

Text drawn by the built-in bitmap font is ASCII only. A glyph such as `✓`
renders as `?`. Draw marks with boxes, borders or `canvas`, or document that
the component needs a runtime TTF font (`packages/engine/rasterized_font.cpp`).

## Keyboard And Accessibility

The engine has no general focus model yet. Today:

- Key events target the focused node (`Tree::activeInputId()`), or the root
  when nothing has focus, and bubble along its path
  (`dispatchRootKeyEvent` in `engine/ui/tree_events.cpp`).
- On the root element of a component, geatsc turns `onKeyDown`/`onKeyUp` into
  a document-level listener. It fires for every key that is not stopped on
  the way (`stopPropagation` on a node suppresses it), and stays registered
  until the app switches. An `input` keeps a per-node listener.
- On a nested element the listener fires only when the focused node is that
  element or inside it: after the user taps an `input` within it, or after
  `element.focus()` on it. With nothing focused it never fires.
- `onFocus`/`onBlur` are not delivered on any native target.
  `focusin`/`focusout` fire when a text input is tapped, when a tap clears
  focus, or when `element.focus()` is called, but there is no Tab order or
  focus ring.
- No native target uses `role`, `aria-*` or `tabIndex` for accessibility.
  macOS reads `role="slider"` only to find a slider's thumb travel.

Until core#5 lands, components do not attach key handlers. A component
documents its keyboard and screen-reader behavior as a known limitation
instead.

## Tests And Evidence

A PR includes:

1. **Usage example.** An app in
   [geastack/examples](https://github.com/geastack/examples) under
   `apps/<name>/` that exercises every prop, opened as a companion PR.
2. **Behavior test.** A native pipeline test in `packages/core/test`, modeled on
   `run-gea-virtual-list-pipeline.sh` and `test_gea_virtual_list_main.cpp`: build
   the example app, drive it with `dispatchTouch` or `clickFirstText`, and check
   layout, pixels and text after each interaction. A function component's
   nodes are replaced when it re-renders, so look them up again after each
   interaction. To test a CSS animation that a class change starts, call
   `StyleSheet::instance().startCssAnimations(now)` before each `pumpFrame`, as
   `packages/core/runtime.cpp` does on the device; the test host's
   `pumpFrame` alone does not start new tracks.
3. **Documentation.** The README bullet, the `.d.ts` comments, the cleanup
   method if there is one, and the stated target scope.
4. **Target matrix.** One row per target, each marked with how it was
   established:

   | Level | Meaning |
   |---|---|
   | tested on device | ran on named hardware |
   | tested in simulator | ran in the WASM simulator (`--renderer wasm`) or the native harness |
   | tested in browser | ran on the DOM target only; not evidence for a native target |
   | source-only | read the code path; not run |
   | unsupported | known not to work, with the reason |

   Source inspection alone does not establish runtime support.
5. **Limitations.** What does not work yet, with a link to the issue that would
   fix it.

## Existing Components Against This Contract

| Rule | `VirtualList` | `CameraView` |
|---|---|---|
| Placement | `elements/components`, backend `elements/ui/virtual_list.cpp` | `elements/components`, backend `elements/ui/camera_element.cpp` |
| Shape | function component | class component; owns the camera |
| Cleanup | none needed | `close()`, called by the app |
| State | `scrollTop` prop that places rows; the native element owns the scroll offset | no value state |
| Props | `id`, `class`, `itemClass` | `facing`, `mirror`, `device`, `width`, `height`, `fit`; styling through `style` only, no `class` or `id` |
| Events | native touch and scroll events | typed `onReady(cam)`, `onError(message)` |
| Test | `run-gea-virtual-list-pipeline.sh` with `apps/virtual-list`, which uses the `<virtual-list>` tag directly | none; `apps/camera-studio` uses the `<camera>` tag and its test only counts nodes |
| Gaps | `slotCount` is declared but ignored; the pool is fixed at 16 | no `class`/`id`; `close()` is declared in the `.d.ts` without a comment and not mentioned in the README; no app or test uses the component |

Target support for these two, from reading the renderers (source-only): both
have backends in the pixel engine. macOS hosts `virtual-list` in an
`NSScrollView`, iOS in a native scroll container and Windows in a native
scroll window. iOS has a camera preview backend (`ios_camera.mm`); macOS and
Windows have none.
