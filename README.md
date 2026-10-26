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
| `packages/host` | Scaffold plus host service implementations and platform abstractions. |
| `packages/engine` | Scaffold plus rendering, font, canvas, image, and input engine sources. |
| `packages/elements` | Scaffold for non-intrinsic JSX elements and component library extraction. |
| `packages/chips` | `@geastack/chips`: target-independent drivers plus the catalog consumed by the CLI and target adapters. |
| `LICENSES` | The GPL-3.0-only text `packages/chips` is licensed under; everything else in this repo is Apache-2.0 (`LICENSE`). |
| `packages/core/gea_sources.sh` | Shared source list helper used by target build scripts. |
| `docs` | Core architecture and development notes. |

## Quick Start

Install dependencies where package manifests exist:

```sh
cd packages/core
npm install
```

Run representative checks:

```sh
cd packages/core
node test/test_gea_app_manifest.mjs
./test/run-tests.sh
```

Some tests compile or read applications that live outside this repository, in
[geastack/examples](https://github.com/geastack/examples) or your own project.
Run the suite from that project's directory; the tests use the apps in the
current directory. From a directory without an `apps/` folder they print `SKIP`
and pass:

```sh
cd /path/to/examples && bash /path/to/core/packages/core/test/run-tests.sh
```

Build/test the Gea compiler plugin:

```sh
cd packages/geatsc-plugin-gea
npm install
npm run build
npm test
```

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
| `@geastack/core`, `engine`, `host`, `elements`, `geaos`, `geatsc-plugin-gea` | Apache-2.0 |
| `@geastack/chips` (board drivers, embedded only) | GPL-3.0-only |

The framework, the compiler and the desktop, mobile and web targets are
Apache-2.0: build and ship closed-source apps on them freely. The embedded
board support (`@geastack/chips` here and the `targets` repo) is GPL-3.0-only,
so shipping closed-source firmware needs a commercial license. Contact
[contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
