# Core Development Guide

This guide covers day-to-day framework work in the core repo.

## Package Commands

Build the Gea compiler plugin first; the core native tests compile with it:

```sh
cd packages/geatsc-plugin-gea
npm install
npm run build
npm test
```

Then the public core package:

```sh
cd packages/core
npm install
./test/run-tests.sh
```

The repository [README](../README.md#what-a-clone-of-this-repo-alone-covers)
lists the tests that skip without the sibling repositories and how to run them.

The scaffold packages do not all have build scripts yet. Treat their
`package.json` files as ownership markers until implementation is moved in.

## Test Strategy

Use focused tests whenever possible:

- build tooling and generated sources: `packages/core/test/test_*.mjs`;
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
