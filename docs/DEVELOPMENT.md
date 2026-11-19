# Core Development Guide

This guide covers day-to-day framework work in the core repo.

## Package Commands

For the public core package:

```sh
cd packages/core
npm install
```

The native pipelines (`test/run-gea-*-pipeline.sh`) compile apps from
[geastack/examples](https://github.com/geastack/examples), so
`test/run-tests.sh` is started from a checkout of that repo, as
`test/native-test-common.sh` describes. From any other directory the
pipelines print `SKIP` and exit 0.

```sh
cd /path/to/examples
bash /path/to/core/packages/core/test/run-tests.sh
```

For the Gea compiler plugin:

```sh
cd packages/geatsc-plugin-gea
npm install
npm run build
npm test
```

The scaffold packages do not all have build scripts yet. Treat their
`package.json` files as ownership markers until implementation is moved in.

## Test Strategy

Use focused tests whenever possible:

- launcher behavior: `packages/core/test/run-gea-app-launcher-pipeline.sh`;
- native pipeline behavior: `packages/core/test/run-gea-*-pipeline.sh`;
- rendering or input behavior: source-specific shell tests under
  `packages/core/test`;
- plugin code generation: tests under `packages/geatsc-plugin-gea/test`.

Run broader shell tests before landing changes that affect shared runtime,
layout, event, rendering, or generated app behavior.

## Generated Artifacts

Generated build folders such as `.build-test`, generated app output, and native
build products are disposable. Do not rely on generated output as source unless
a test fixture explicitly owns it.

## App Manifest Tools

App discovery and manifest validation live in
[geastack/cli](https://github.com/geastack/cli): `src/manifest.mjs` and
`validateBuildManifest` in `src/build-config.mjs`. The launcher catalog is
`src/apps/launcher-catalog.mjs` there. Keep manifest changes aligned with the
examples repo.

## Change Checklist

Before landing core changes:

1. Identify the owning layer from [ARCHITECTURE.md](ARCHITECTURE.md).
2. Keep imports and package metadata aligned with that layer.
3. Run the focused tests for the touched behavior.
4. Run at least one representative app pipeline if runtime/rendering changed.
5. Update docs when package boundaries, commands, or target expectations change.

## Common Pitfalls

- Do not add target-specific ESP-IDF, AppKit/UIKit, GeaOS, or web behavior to a
  generic framework package.
- Do not add Gea UI or device assumptions to the generic compiler runtime.
- Do not change a package boundary without updating source manifests used by
  targets.
- Do not break existing example imports without updating examples in the same
  work.
