# GeaStack Core

Core framework packages for Gea applications.

This repo owns the TypeScript-facing Gea app API, Gea runtime sources, UI
engine scaffolding, host abstractions, reactivity internals, and the Gea
`geatsc` plugin. It is the framework layer between the compiler, example apps,
simulators, and physical targets.

## What Is Here

| Path | Purpose |
| --- | --- |
| `packages/core` | Public `@geastack/core` package with type declarations, runtime helpers, reactivity headers, app lifecycle/frame-loop sources, tools, scripts, and tests. |
| `packages/geatsc-plugin-gea` | Gea IR backend plugin for `geatsc`. |
| `packages/host` | `@geastack/host`: the C++ host contract and the portable host service implementations. |
| `packages/engine` | `@geastack/engine`: intrinsic JSX primitives and the rendering, layout, style, font, canvas, image and input engine sources. |
| `packages/elements` | `@geastack/elements`: non-intrinsic JSX elements (`VirtualList`, `CameraView`) with their native backends. |
| `packages/geaos` | `@geastack/geaos`: installed-application runtime sources (app manager, installed-app registry) for targets that host more than one app. |
| `packages/daily` | `@geastack/daily`: work-in-progress native Daily WebRTC transport. Private, not published. |
| `packages/chips` | `@geastack/chips`: target-independent drivers plus the catalog consumed by the CLI and target adapters. |
| `LICENSES` | The GPL-3.0-only text `packages/chips` is licensed under; everything else in this repo is Apache-2.0 (`LICENSE`). |
| `packages/core/gea_sources.sh` | Shared source list helper used by target build scripts. |
| `docs` | Core architecture and development notes. |

## Quick Start

Requires Node.js 20.19 or newer, `clang++` with AddressSanitizer, and
`pkg-config` with the `opus` and `vpx` development libraries (on Debian or
Ubuntu: `sudo apt install clang pkg-config libopus-dev libvpx-dev`).

Build and test the Gea compiler plugin first; the core native tests compile
with it:

```sh
cd packages/geatsc-plugin-gea
npm install
npm run build
npm test
```

Then install core's dependencies and run its suite:

```sh
cd ../core
npm install
./test/run-tests.sh
```

The suite compiles against the `@geastack/compiler` that `npm install` puts in
`packages/core/node_modules`, or against a `compiler` checkout next to `core`
when there is one. Set `GEA_COMPILER_CLI` and `GEA_COMPILER_RUNTIME` to use
another compiler.

### What a clone of this repo alone covers

Every test in `packages/core/test` runs from a clone of this repo alone,
except these, which print a `SKIP:` line naming what is missing and pass:

| Tests | Need |
| --- | --- |
| `run-gea-*-pipeline.sh` and the other example-app pipelines, `test_bitmap_font_source.mjs` | the apps in [geastack/examples](https://github.com/geastack/examples): run the suite from an examples checkout |
| Apple parts of `test_desktop_websocket_sources.mjs`, `run-macos-*` and `run-native-text-decoration-source-test.sh`, `run-gea-companion-*` | `GEA_APPLE_ROOT` set to the `@geastack/apple` package root (`apple/packages/geastack-apple` in a [geastack/apple](https://github.com/geastack/apple) checkout); `run-gea-companion-*` link Apple frameworks and also need macOS |
| Linux parts of `test_desktop_websocket_sources.mjs` | a [geastack/linux](https://github.com/geastack/linux) checkout next to `core` |
| Web target part of `test_web_build_sources.mjs` | a [geastack/simulator](https://github.com/geastack/simulator) checkout next to `core` |
| `run-native-typed-websocket-retirement.sh` | a [geastack/compiler](https://github.com/geastack/compiler) checkout next to `core` (it compiles one of the compiler's own test sources) |

To run the full set, clone those repos next to this one and run the suite from
the examples checkout:

```sh
git clone https://github.com/geastack/examples
git clone https://github.com/geastack/apple
git clone https://github.com/geastack/linux
git clone https://github.com/geastack/simulator
git clone https://github.com/geastack/compiler   # optional: test against the compiler sources
(cd examples && npm install)
cd examples
GEA_APPLE_ROOT="$PWD/../apple/packages/geastack-apple" bash ../core/packages/core/test/run-tests.sh
```

A sibling `compiler` checkout is used as is, so build it first
(`npm install && npm run build` in `compiler`).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): framework layers, package
  responsibilities, and source-fusion boundaries.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): test strategy, source layout, and
  change checklist.
- [docs/COMPONENTS.md](docs/COMPONENTS.md): what a UI component contribution to
  `@geastack/elements` must include.

## Maintenance Notes

- Keep public app-facing types in `packages/core` stable while the split
  packages are being populated.
- Keep generated app/tool helpers behind package scripts or tools.
- Do not move target-specific ESP32, Apple, GeaOS, or web implementation into
  the generic framework layer.
- When package boundaries change, update docs and `gea_sources.sh` together.

## License

| Package | License |
| --- | --- |
| `@geastack/core`, `engine`, `host`, `elements`, `geaos`, `geatsc-plugin-gea`, `daily` | Apache-2.0 |
| `@geastack/chips` (board drivers, embedded only) | GPL-3.0-only |

The framework, the compiler and the desktop, mobile and web targets are
Apache-2.0: build and ship closed-source apps on them freely. The embedded
board support (`@geastack/chips` here and the `targets` repo) is GPL-3.0-only,
so shipping closed-source firmware needs a commercial license. Contact
[contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
