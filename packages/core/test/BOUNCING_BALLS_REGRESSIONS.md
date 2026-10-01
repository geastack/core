# Bouncing Balls performance regressions

These tests protect the behavioral failures found while restoring the original
Geastack Bouncing Balls JSX app from 11.495 to 60.132 completed frames/second on
the USB AMOLED 2.06. They deliberately assert work counts and rendering results,
not desktop timing. Shared styles remain disabled in this baseline.

Run from the core repository:

```sh
bash packages/core/test/run-bouncing-balls-regressions.sh --mutations
bash packages/core/test/run-text-sprite-cache.sh --mutations
bash packages/core/test/run-gea-retained-absolute-subtree.sh
cd packages/geatsc-plugin-gea
npm run build
node --test --test-name-pattern='rounded CSS|rounded video|radius|span cache' test/analyze.test.mjs
```

The `--mutations` checks reintroduce the old failures into copies in ignored
native build output. They require the intended behavioral assertion to fail;
compile errors do not count as catching a regression. Working sources and the
shared compiler are never replaced. CI runs these focused commands on relevant
pushes and pull requests.

Coverage:

- CSS percentage, fixed, corner and dynamic radii retain circle span caches;
  provably zero radii and unrounded video still discard them.
- 64 moving leaves, with both unset and explicit-zero minima, keep layout and
  display commands. Every frame performs 128 fast integer position applications,
  zero full layouts, zero rerecords and zero descendant-clip scans. All 64
  siblings resolve their shared containing box once per frame; alternating parent
  groups, borders and percentage anchors are checked against full layout. Constrained
  dimensions retain the general fallback. Geometry and pixels are checked too.
- Signed integer edge positions preserve raw-pixel semantics at DPR 2. Units,
  expressions, fractions, malformed values and range overflow stay on the general
  CSS path. `px` values still scale with DPR.
- Already-clipped replay never submits to a worker that lacks its DMA binding.
  Pixel assertions verify the rebound buffer, active clip, stride and guards.
- Alternating `FPS: 59` / `FPS: 60` warms both cache entries, then performs no
  rasterization or rebuilds. Cached/uncached output matches across clipping and
  alpha. Font identity, font size and bounded four-entry replacement are checked.
- The existing retained-subtree regression checks moving overflow clips and
  translucent overlays, protecting the non-leaf path from the leaf shortcut.

The two additional work counters and their increments are absent when
`GEA_EMBEDDED_UI_REFRESH_PERF=0`. The text test uses existing diagnostics inside
its own translation unit; it adds no public API or firmware instrumentation.

The matching scheduler/DMA tests live in `targets/esp32/test/frame-cadence.test.mjs`
in the targets repository. The physical throughput gate is
`npm run test:balls:device` in the examples repository; see that app's
`PERFORMANCE.md`. A host pass alone is never a 60 FPS claim.

The shared-style experiment also runs this same retained-pixel golden and
behavioral suite with `GEA_BALLS_SHARED_STYLES=1`. Its additional ownership cases
check copy-on-write isolation, interning, inherited changes and reclamation.
Both representations must retain the captured inline framebuffer hash. This is
a host correctness gate; the original JSX app must separately pass the USB
completed-frame performance gate.


Layout memo storage is transient. The retained regression asserts zero scratch
allocation after each frame. The focused memo cases cover two-slot promotion,
external resizing, explicit invalidation, scoped success/rejection cleanup,
node-slot reuse, missing-scratch fallback, serial wrap and tree reset. Both
inline and shared configurations run these same checks. The hardware census
also reports the complete scratch peak and requires zero live scratch after
warm-up; moving bytes out of Node cannot hide their allocation cost.

Shared nodes also keep cold layout state in stable 16-slot pages. Focused checks
cover growth across page boundaries, retained entry references, static-position
anchors, scoped relayout, node reuse and complete reclamation. The device gate
counts usable allocation sizes, allocator headers and overlapping pointer tables
at growth; requested payload alone is insufficient.

When the complete source graph proves a class bound below four tokens, the CLI
also removes the unused overflow handle. A separate versioned proof is required;
old analyzers, unknown classes, mutation APIs, native UI code and incomplete
merged proofs retain overflow support. The compact class suite runs with
`GEA_BALLS_SHARED_STYLES=1 bash packages/core/test/run-bouncing-balls-regressions.sh --compact-classes`.
It checks class copy/move, independent mutation, removal, slot reuse, duplicate
insertion at capacity, clearing, and the original retained pixel golden. An
intentional proof violation must abort instead of silently dropping a class;
the default suite separately checks overflow copy/move and token promotion.

The device scratch census now reports `memo_peak_heap`: the allocator's usable
block size, not requested payload. This uses the existing peak counter and adds
no static storage or retained-frame work.

With `GEA_BALLS_PERF=0`, the same pixel, geometry, class and ownership cases run
without work-count assertions. A sentinel across the actual diagnostic backing
storage must remain untouched, and profiling macro arguments must not execute.
The production engine compiles diagnostic writes and region/cache census scans
out rather than writing counters to a discarded object. Device timing still
comes from the independent completed-frame scheduler gate.

The v16 source analysis also proves whether ten common style families need
storage: flex direction, main/cross-axis alignment, box sizing, auto margins,
unitless line height, deferred widths, min height, max width and active background.
Unknown styles, native defaults and opaque imports retain these fields; CSS
imports, shorthands and dynamic values participate in the proof. Older analyzer
versions cannot prune them. No application opt-in or runtime test is required.

The same native suite checks the fields enabled and with all ten removed:

```sh
GEA_BALLS_BASE_STYLE_PRUNED=1 GEA_BALLS_PERF=0 GEA_BALLS_SHARED_STYLES=1 bash packages/core/test/run-bouncing-balls-regressions.sh --compact-classes
```

Both configurations must retain the same pixel golden. Enabled-field cases
check initial flex direction, authored alignment, content/border-box dimensions,
min/max constraints, deferred-width resets, inherited unitless line heights and
fixed-line-height resets. Pruned fields have compile-time initial values, so
reads gain no indirection. The original JSX app retains deferred width storage
because it uses viewport widths; its device build is checked separately.

The v17 proof additionally removes unused margins, padding, flex factors, gap,
border widths/colors and font weight/text alignment/whitespace/ellipsis fields.
Native/opaque code retains all ten. Intrinsic text tags retain their defaults;
font-size and rounded backgrounds do not accidentally enable unrelated fields.
Existing pruning CI covers this batch too, and the shared pruned row additionally
uses `GEA_BALLS_NARROW_STYLE=1` to cover byte-sized radius/line-height packing.
Enabled-field cases verify signed edges, factors, gap, border color/currentColor,
font/text values and inheritance. The native color test uses the runtime's
supported hex spelling; named-color support is outside this pruning change.


The v18 proof removes custom-property stores and dependency caches when neither
source-visible styles nor opaque/native code can require them. Definitions,
`var()` use, dynamic property names and opaque styles retain support. Dynamic
numeric coordinates alone do not. Both full variable support and its omission
are exercised by the same native suite. It checks ancestor class invalidation,
inline override/removal, fallback resolution, ordinary class/inheritance changes,
and node-slot reuse. A shared node keeps the remaining tracked flag in existing
alignment padding; the inline control retains a one-byte-per-node array.
Compile-time offset assertions ensure the flag does not move RenderState.

`GEA_BALLS_CUSTOM_PROPERTIES=0` selects the proven-unused configuration in these
host tests only; application builds infer it automatically. The existing pruned
CI rows include it. Enabled support remains covered by the default rows.

A read-only allocation census includes the tree owner, text pages/string buffers,
rare-record pool and attributes, override blocks/unit metadata, and variable
dependency globals. It runs after the completed-frame sample. Opaque owning
callbacks/shared strings are explicitly reported as untracked rather than
silently omitted. The device gate requires every ownership group and rejects an
incomplete census. Override count/capacity narrowing is checked through growth to
every valid Property, independent copy/unit metadata, erase, and move.
