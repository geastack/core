# @geastack/core

The app-facing GeaStack API: the type declarations for Gea JSX elements,
styles and events, the TypeScript runtime helpers, and the C++ framework
runtime that every native target compiles in.

```sh
npm install @geastack/core
```

## How it is used

Apps import the types and the runtime modules:

```tsx
import type { Style } from "@geastack/core";
import { View, Text } from "@geastack/engine";
```

Everything else in the package is consumed by tooling, not by app code. The
`gea` CLI and each target's build script resolve `@geastack/core` from the
app's `node_modules`, read its source manifest to learn which framework sources
and include roots to compile, and run its build driver, which bundles the app
with Vite, compiles it with `geatsc` and the Gea plugin, and hands the emitted
C++ to the target.

## Component startup

Use `@geastack/core` for applications targeting the native GeaStack hosts.
`Component` is the component base; `ReactiveComponent` makes its own fields
reactive, and `Store` holds shared reactive state. The web target supplies the
matching browser runtime. Direct use of `@geajs/core` belongs to the Gea web
framework and is not the native application contract.

Start loading data or drawing in `onAfterRender()`. The component's `el` is set
before this hook runs. Field initializers and constructors initialize state;
code that needs a rendered canvas belongs in the hook.

```tsx
import { ReactiveComponent } from "@geastack/core";

class Counter extends ReactiveComponent {
  count = 0;
  onAfterRender(): void {
    this.count = 1;
  }
  template(): JSX.Element {
    return <div>{this.count}</div>;
  }
}
```

Compiler/runtime regressions should use generic language or framework cases.
Report compiler issues in `geastack/compiler`, host API and application-runtime
issues in `geastack/core`, Apple rendering/bindings in `geastack/apple`, and web
target integration in `geastack/simulator`. Contributions should include a
reproduction and target-specific validation. Lifecycle and representation
changes need to preserve the shared component and native-type contracts.

## Native WebSocket connection headers

`@geastack/core` 0.1.41 and `@geastack/host` 0.1.16 add an optional third
WebSocket constructor argument for connection headers on ESP32 native hosts:

```ts
const socket = new WebSocket("wss://api.openai.com/v1/live/sessions", "", {
  headers: [
    { name: "Authorization", value: `Bearer ${process.env.OPENAI_API_KEY}` },
  ],
});
```

Use these paired versions or newer when a service requires bearer authentication
in the WebSocket handshake. Header names and values are validated before the
connection starts. This native option is unavailable in browser WebSockets.

## File-backed native assets

Native apps can keep images and baked font atlases on a mounted volume instead
of in the executable. Opt in through the app manifest:

```json
{
  "gea": {
    "embeddedAssets": {
      "storage": { "type": "file", "directory": "/nand" }
    }
  }
}
```

The generators emit content-addressed `.bin` payloads and a `.assets.json`
deployment manifest alongside each generated C++ source. Copy each `local` file
to its `remote` path, for example with `gea devctl push`, before starting the app.
The target must register a mount provider; ESP32 targets also need external BSS
enabled. The runtime reads and validates the entire bundle once into permanent
RAM buffers. Render-time image and font lookups use those buffers, with no file
I/O. Missing, truncated or corrupt bundles are rejected until the next process
start. Retain older content-addressed files when firmware rollback is required.
Apps without this option continue embedding their assets in the executable.

## Physical buttons

A board's buttons reach the app as key events named after the button:

```tsx
<div onKeyDown={e => { if (e.code === 'Boot') openMenu() }}
     onKeyUp={e => { if (e.code === 'Power') confirm() }} />
```

`e.code` is the silkscreen name in PascalCase (`'Boot'`, `'Power'`). `e.key` and
`e.keyCode` are the button's default key alias (BOOT is `Escape`/27 and PWR is
`Enter`/13 on the Waveshare S3 boards), so an app that only handles keys still
works on every board. `Buttons` lists what the board has, before any press:

```ts
import { Buttons } from '@geastack/core'
for (let i = 0; i < Buttons.count(); i++) console.log(Buttons.code(i), Buttons.key(i))
Buttons.pressed('Boot') // held right now?
Buttons.pin(0)           // its GPIO, or -1 for a PMU power key
```

Buttons are declared as JSON. A board lists its own under `buttons`, in its
`targets.json` entry, its composed board definition, or its alias in
`boards.json`:

```json
"buttons": [
  { "code": "Boot",  "pin": 0,  "activeLevel": 0, "key": "Escape" },
  { "code": "Power", "source": "pmu", "key": "Enter" }
]
```

An app adds, changes or removes buttons by name in its `package.json`, for every
board (`gea.targets.esp32.buttons`) or for one
(`gea.targets.esp32.boards.<id>.buttons`). Use this, for example, for a button
wired onto a header pin:

```json
"gea": { "targets": { "esp32": {
  "buttons": [
    { "code": "Fire", "pin": 14, "activeLevel": 0, "key": "Enter" },
    { "code": "Power", "remove": true }
  ]
} } }
```

`pin` is a GPIO number. `activeLevel` is the level the pin reads while the button
is held (default 0); an active-low button gets the internal pull-up, an
active-high one the pull-down. `key` is the web key name the button stands in
for, which also sets `keyCode`; give `keyCode` only for a key outside the common
set. `"source": "pmu"` is the AXP2101 power key. The build checks every entry and
names the field that is wrong.

GPIO buttons are interrupt-driven: one task debounces every edge. A button is
polled only when its entry says `"poll": true`, for example a pin whose
interrupt something else owns. The AXP2101 power key is polled on boards that
leave the PMU's IRQ pin unconnected, because it has no interrupt to wait on. A
RESET key is the chip's enable line, which no software can see.

Presses join the ordered hardware-key queue the frame loop already drains. They
bubble through the UI tree, and focus and `preventDefault` work as for any key.
`code` and `key` are plain native strings on the event, so nothing is boxed.

## Digital pins: `Gpio` and `Digital`

Two classes drive and read pins. Use whichever you prefer; they are two TS
front ends over one native backend (`host/gpio.h`, `host/gpio.cpp`) and behave
the same way underneath.

- `Gpio` is the GeaStack class: string options, boolean levels, and an
  `onChange(value)` callback.
- `Digital` is the ECMA-419 Digital IO class with Moddable's mode numbers.
  Code written for another ECMA-419 host runs unchanged.

```ts
import { Digital, Gpio } from '@geastack/core'

// Gpio
const led = new Gpio(2, { mode: 'output' })
const key = new Gpio(14, {
  pull: 'up',
  onChange: (high) => led.write(!high), // pressed pulls the pin low
})

// ECMA-419 Digital
const button = new Digital({
  pin: 15,
  mode: Digital.InputPullUp,
  edge: Digital.Rising | Digital.Falling,
  onReadable() {
    console.log(this.read()) // `this` is the Digital
  },
})
```

### Interrupts by default

An input with a callback is interrupt-driven. The pin's edge interrupt does
nothing but mark the pin. The callback then runs on the app thread at the start
of the next frame: after pending microtasks, before `requestAnimationFrame`
callbacks. State it changes therefore renders in that same frame. Several
edges within one frame give one call. The callback reads the level at that
moment, so it always sees where the pin is now.

To poll instead, pass `poll: <milliseconds>`, for example for a noisy line you
would rather sample than interrupt on. Polling also covers a platform or pin
with no edge interrupt; such a pin is read once per frame.

```ts
new Gpio(14, { pull: 'up', onChange, poll: 20 })
new Digital({ pin: 15, mode: Digital.Input, edge: Digital.Falling, onReadable, poll: 20 })
```

There is no debouncing; the level is read when the callback runs.

- `Gpio` with both edges (the default whenever `onChange` is given) reports
  only real changes, so a bounce that settles back where it started reports
  nothing.
- A pin watching one edge reports when it has settled on that edge's level:
  high for `rising`, low for `falling`.
- `Digital` calls `onReadable` once for each frame that saw an edge it asked
  for, and `this.read()` gives the level.

### `Gpio`

`new Gpio(pin, options?)` opens the pin and throws if it cannot be opened. That
happens for a pin the chip does not have, a pin that is already open, or a
target with no GPIO.

| Option | Values | Default |
| --- | --- | --- |
| `mode` | `'input'`, `'output'`, `'openDrain'` | `'input'` |
| `pull` | `'none'`, `'up'`, `'down'`, `'both'` (inputs) | `'none'` |
| `edge` | `'none'`, `'rising'`, `'falling'`, `'both'` | `'both'` when `onChange` is given, else `'none'` |
| `onChange` | `(value: boolean) => void` | none |
| `poll` | milliseconds | interrupts |

Methods:

- `read()` returns the level; for an output, the level it drives.
- `write(value)` drives an output. It throws on an input.
- `close()` releases the pin and its interrupt. Closing from inside the pin's
  own callback is allowed.
- `pin` holds the pin number.

The older static calls `Gpio.configureOutput(pin)`, `Gpio.configureInput(pin,
pullUp)`, `Gpio.write(pin, level)` and `Gpio.read(pin)` still work. They answer
"did this board do it" with a boolean instead of throwing.

### `Digital` (ECMA-419)

`new Digital({ pin, mode, edge?, onReadable?, target?, format?, poll? })`.

- **Modes:** `Digital.Input`, `InputPullUp`, `InputPullDown`,
  `InputPullUpDown`, `Output` and `OutputOpenDrain`.
- **Edges:** `Digital.Rising` and `Digital.Falling`, OR'd together for both.
  `edge` is required with `onReadable`, and an output takes no edge.
- **Methods:** `read()` returns 0 or 1, `write(0 | 1)` drives an output, and
  `close()` releases the pin.
- **`target`:** kept as `this.target` for the callback. Its type comes from the
  value you pass, so it stays native.
- **`format`:** only `'number'` is supported.
- **`poll`:** a GeaStack extension; see above.

Bad options throw `RangeError`, as the standard says. A pin that cannot be
opened throws `Error`.

### Board buttons

A pin the board declares as a button (see above) can also be opened as an
input, for an app that wants levels rather than key events. The pin then keeps
the button's own setup and interrupt, so key events keep arriving.
`Buttons.pin(i)` tells you which GPIO a button is on. A polled button's pin is
polled by `Gpio`/`Digital` too.

### Where pins work

ESP32 targets drive real pins through ESP-IDF, with up to 32 pins open at once.
On other targets (desktop, mobile, the simulator) there is no GPIO. There,
`new Gpio()` and `new Digital()` throw, and the static calls return false. An
app that wants to run on both should catch the error.

## Related packages

- `@geastack/engine` renders the element tree, `@geastack/host` supplies the
  platform services, `@geastack/elements` adds the non-intrinsic elements.
- `@geastack/geatsc-plugin-gea` is the `geatsc` backend the build driver loads.
- `@geastack/compiler` is `geatsc` itself.

## License

Apache-2.0. See `LICENSE`.
