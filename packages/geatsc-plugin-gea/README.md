# @geastack/geatsc-plugin-gea

The Gea backend plugin for `geatsc`, the GeaStack TypeScript-to-C++ compiler.
It teaches the compiler the Gea framework: which globals are host functions,
how reactive stores and components lower, and how the app's Gea IR becomes C++.

```sh
npm install @geastack/geatsc-plugin-gea
```

## What it does

- Declares the Gea host surface so calls into the framework compile to the
  runtime's C++ entry points instead of being refused as unknown globals.
- Lowers the Gea IR that the Vite build emits into the C++ that mounts the
  element tree, wires stores and reactive components, and applies styles.
- Analyzes the app source for the host bindings it actually uses so the target
  compiles only what the app needs.

## How it is loaded

`@geastack/core`'s build driver resolves this package and passes it to the
compiler; an app never references it directly. The same can be done by hand:

```sh
geatsc compile-module-graph gea-module-graph.json --entry src/index.tsx \
  --out-dir build/generated --plugin @geastack/geatsc-plugin-gea
```

The module's default export is the plugin factory, as described in the
compiler's `docs/CLI-PLUGINS.md`. `@geastack/geatsc-plugin-gea/host-shims` and
`@geastack/geatsc-plugin-gea/cpp-ir` are exported for tools that need the
tables without the plugin.

## License

Apache-2.0. See `LICENSE`.
