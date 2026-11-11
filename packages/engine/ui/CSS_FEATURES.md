# Automatic CSS specialization

The compiler plugin reports CSS families reachable from application source, imported
stylesheets and dynamic style operations. The CLI combines this with native-source
usage before generating consistent engine and application definitions. Missing or
opaque analysis retains support. Applications need no feature switches.

The first families are transforms/perspective, grid, floats, writing modes/direction
and relief borders. Per-property analysis also omits unused blink timing, flex/grid
order, and percentage representations for gaps and corner radii. Dynamic values
retain the representations they can reach. Missing/older analysis retains defaults;
the CLI requires v2 before specializing those representations. The v3 facts also
omit unused opacity, text decoration/transformation, visibility, pointer-events,
masking and object-fit fields. Attribute-driven native animations and opaque native
CSS APIs retain support conservatively. Removing a family removes its per-node state and allows the
optimizer to discard its paths. Ordinary borders and border-box sizing stay usable.
The v4 facts additionally remove unused filters, box shadows, flex wrapping,
justify-items, align-content/self, min-width and deferred height expressions.
Logical size properties, unknown values and native CSS mutation retain the relevant
families. Shared CSS length parsing stays outside the transform-only guards;
font-size, padding and min/max sizes must work with transforms absent.
Renderer cache analysis separately removes unused persistent caches while retaining
uncached rendering paths.

Empty nodes store a four-byte text handle. Nonempty labels own independent strings
in stable pages of 16 records; releasing a label destroys its character buffer and
reuses the record. Pool pages retain their high-water capacity and prefer PSRAM on
ESP32. NodeText::storageBytes reports allocated pool payload, including spare records
and the page map, but excludes long string buffers and allocator metadata.

The pedal's linked S3 ELF measures 220-byte nodes and a 39,472-byte TreeState
for 160 slots, versus 400 and 68,272 with engine 0.1.6. Node members remain
naturally aligned direct fields; absent properties become compile-time constants.
The width probe still emits one S3 load, with no accessor or pool lookup. Total UI memory includes the
text pool and other allocations; these are not device heap or timing measurements.

Native applications that only inspect the mounted tree can include
ui/tree_inspection.h. Mutable UI headers conservatively retain all CSS families.

The common border color's binding flags occupy existing ComputedStyle alignment
padding, so an ordinary literal colored border does not allocate RareStyle.
Per-side color payloads remain sparse. In the S3 specialization, RareStyle is
128 bytes (336 in the registry baseline). The native regression checks
both the primitive binding and the CSS shorthand/side-color/currentColor path.
These record sizes exclude the pool allocator's block and bookkeeping overhead.

The optional node record's style overrides share storage between inline entries
and overflow metadata, which are mutually exclusive. On S3 each override store
falls from 48 to 36 bytes and NodeRareData from 280 to 256 bytes. Its access
function has the same instruction sequence, with changed offsets only. Copies,
moves, overflow growth, removal and reuse retain their previous semantics.

Optional records now use an owning pointer table: record addresses remain stable
across growth and freed records are reused. This avoids deque indexing becoming
more expensive when a smaller record crosses a block-size threshold. The linked
S3 lookup is 52 bytes versus 90 before the pointer-table follow-up, with fewer loads/branches;
that is an assembly observation, not a device timing measurement. Allocations
include the record payloads, spare pointer-table capacity, reuse list, nested
containers and allocator overhead; record sizeof alone is not total heap use.

An allocator-level macOS regression compares the installed registry engine and the
candidate across six phases of a 49-node fixture. It sums all malloc zones, counting
nested containers, long text buffers, spare capacity and allocator rounding. The
attribute/listener/inline-style phase saves 68,576 allocated bytes; releasing long
labels saves 83,936 against the registry engine, which retains string capacity.
Pixels match in all phases. These totals include host framework state and are not
S3 heap readings or a whole-pedal workload. The test requires explicit reference
and candidate package paths and is intentionally separate from ordinary host checks.

Expanded class-rule coverage exposed a transform guard that had accidentally
removed shared length parsing in the earlier development candidates. The current
candidate corrects it and tests font-size, line-height and min/max/box edges with
transforms disabled. Earlier 288/260/252-byte development artifacts are superseded
and must not be treated as broadly behavior-qualified release candidates.

Grouping the existing byte-sized paint flags removes four alignment bytes from
ComputedStyle without bitfields or packed/unaligned members. S3 width and colour
probes remain single aligned loads (l16si/l16ui). ComputedStyle is 120 bytes;
linked firmware is 3,125,088 bytes versus 3,224,448 for the registry baseline.
The host allocator census savings remain unchanged because of host layout and
allocation rounding; do not add the S3 sizeof reduction to that host result.

The v5 analyzer additionally specializes z-index, aspect-ratio, margin-trim,
containment, justify-self, flex-line-count and deferred margin/padding expressions.
Unknown/dynamic values retain the required representation. Fixed pixel box edges
keep ordinary layout behavior without expression arrays or resolver calls. All
versions before v5 retain these newer fields. Grid's authored-template and layout
bookkeeping is also omitted from NodeRareData when grid is unreachable.

For the pedal this leaves Node at 232 bytes, while RareStyle falls from 176 to
128 bytes and NodeRareData from 256 to 244 bytes. The linked firmware is 3,114,768
bytes. Host allocator savings in the 49-node fixture remain unchanged: that
fixture does not allocate rare styles, and the smaller optional records remain
in the same host allocation size class. Device allocation effects must be measured.


The source analyzer now resolves the narrow colour-only custom-property case
across all discovered files before retaining gradient caches. A whole-value
`var(--name)` can omit those caches only when every visible declaration is a
literal colour and every fallback is also a literal colour. Missing definitions,
variable aliases, compound backgrounds, interpolated CSS, opaque writes/imports
and native CSS mutation retain the caches. Custom-property names stay
case-sensitive; ordinary CSS property names do not. This adds build-time analysis,
not runtime field access or decoding. The focused analyzer has 122 passing cases;
cached/uncached native pixels and geometry match, with cache storage absent in
the pruned binary. The pedal image falls a further 4,368 bytes to 3,110,400, and
static PSRAM falls another 5,168 bytes. Node stays 232 bytes.

Hardware qualification is still open. On the connected S3, the preceding
`candidate-sizing` increased maintenance free PSRAM by a median 190,556 bytes
and free internal SRAM by 512 bytes versus the registry build. The PSRAM change
includes a 131,072-byte reduction in mapped code/rodata pages and a 25,952-byte
static-BSS reduction; it must not be described as isolated UI-tree allocation.
Two registry audio windows (45 seconds, A2-Full, amp and all six effects enabled)
had zero deadline misses. The sizing candidate had 55/1 and 0/2 misses on the two
stages. It therefore fails audio qualification despite the memory savings. The
colour-cache candidate needs its own measurements. No new package is published.


Hardware follow-up: the colour-cache candidate's two stage-B misses were traced
to the audio test's full task census. ESP-IDF scanned PSRAM task stacks while
holding its scheduler lock for 2,200/1,792 us. Reading only persistent task
metadata without stack scans reduces this to 114/112 us; DSP counters are now
captured before shutdown diagnostics. Two corrected 45-second windows report
0/0 deadline misses and zero USB transfer errors. The resulting development
image is 3,110,736 bytes, and maintenance free PSRAM rises by a five-sample median
195,640 bytes versus registry. DSP stage time is essentially unchanged. The
previous sizing image's 55 core-0 misses remain a rejected result; this does not
retroactively qualify it. No sample-by-sample device audio or busy-UI equivalence
is claimed, and no further package has been published. The maintained pedal
report is docs/UI_MEMORY_OPTIMIZATION.md; raw measurements are in its
build/css-feature-audit/device-node-compaction-comparison.json.


The v6 facts distinguish independent row/column gaps and independent corner
radii. Uniform literal radii use one aligned int16 field; all rendering indices
become the compile-time constant zero. Uniform gaps omit the unused axis fields.
Unknown values/functions and individual longhands keep the full representation,
and percentage storage remains independent. The S3 node becomes 220 bytes and
TreeState 39,472 bytes. Direct loads are preserved; a dynamic corner read loses
its index arithmetic. Five board samples show 1,964 additional free PSRAM bytes
versus the 232-byte checkpoint; the host allocator rounds to the same size class.
Uniform radii also emit one style write/cache entry instead of four; class-rule
cascade and inline removal are compared across full and pruned configurations.

The radius-write follow-up reduces the firmware another 496 bytes to 3,108,992.
Seven native configurations match across eighteen pixel/geometry frames; plain
and rounded allocator fixtures match registry pixels with no additional host
allocated-byte savings. The final image has 4,027,832 median free PSRAM bytes
(+197,608 versus registry), with the extra four bytes versus uniform fields
inside measurement noise. A 45-second steady full-chain run and a 90-second
real-touch soak both report zero DSP misses and USB errors. Steady DSP times
remain about 1,154/1,194 us; matched touch repaint times are broadly unchanged.
The soak's legacy panel points exercise a subset and toggle the amp/effects;
its quiet-time heuristic can miss delayed paints. Full-CSS WASM and pedal host
checks pass. This remains an unpublished checkpoint toward the original goal,
not proof of hardware sample equality or a final registry-only package build.


The combined storage work keeps computed style, layout and render fields direct.
It replaces seven listener vectors with one tagged collection, right-sizes
attribute payloads, allocates override blocks and virtual-list state only when
needed, and shares the valid result dimensions of the two layout memo entries.
The linked S3 checkpoint has a 212-byte Node and a 72-byte optional record.
A repeated device UI soak exposed a startup/buffer-disruption concern; it is
not release-qualified. See the pedal report for the negative measurements.

Analyzer v8 adds automatic side-border, background-layer and line-height-expression
specialization. Proven uniform borders need no per-side storage or rare-style
lookup; plain colors need no clip/placement/layer-count fields. Whole-value color
variables use the same conservative proof as gradient-cache analysis. Relative
line heights, non-flat backgrounds, side longhands, unknown values and opaque
native CSS retain their support. Old analyzers cannot remove these new fields.
The fully pruned native fixture has a 4-byte RareStyle (deferred flex basis only).

Custom-property records now reuse the stable immutable string already stored in
the CSS atom table, without a second string allocation or reference-count control
block for interned values. If the table is full, an owned shared string preserves
the exact value. Copies, mutation, empty values, cached color/length fields and
self-assignment after atom exhaustion are exercised. This changes cold variable
storage, not direct computed-style field reads. Total allocated bytes, including
fallbacks, are measured separately from record sizes.


Analyzer v9 retains scrolling for scrollable/unknown overflow, scroll controls,
local background attachment, native lists/inputs, dynamic or escaped element
factories and opaque CSS/native writes. A proven non-scrolling app keeps clipping
but omits six 32-bit scroll geometry fields, the scroll-dirty byte, child-extent
walks and pre-layout offset snapshots. Byte flags are grouped before aligned
colors/bounds, without bitfields or field accessors. Default/older-analyzer
builds retain scrolling and its 32-bit large-list coordinates. The native
full/pruned matrix compares hidden/clip/visible overflow through updates; a
separate retained-scroll regression checks scrollIntoView and offset changes.


Analyzer v10 separately proves deferred flex bases and custom-property length
caches unused. Fixed pixel/numeric bases retain direct scalar storage; relative
units, percentages, functions and unknown values retain expressions. Color-only
or quoted-text custom properties need no parsed length fields. Other values,
older analyzers and opaque native mutation retain them. This is automatic.

When every rare family is absent, RareStyle has no instance fields: nodes carry
no rare-style handle and allocate no rare-style pool, including inline builds.
An emptiness assertion prevents a newly added unguarded field from silently
escaping the family list. Unused scrolling also removes tree-level dirty words,
pending-scroll state and the optional virtual-list pointer. Reordering the two
atom IDs removes padding from custom-property records in full builds too.
These changes preserve direct hot-field reads; allocation and target measurements
remain separate from sizeof results.


Analyzer v11 adds maximum-height, scalar flex-basis and independent overflow-axis
proofs. Maximum-height declarations and logical maximum sizes retain their field;
explicit/unknown flex bases retain the scalar basis. Uniform overflow shorthands
share the existing aggregate field; longhands, unequal/unknown values and native
inputs/lists retain separate axes. The existing overflow helpers then compile to
one direct read instead of coupling two axes at runtime. Gradient-only `bg_fill`
and unused transform-scan cache state follow their existing feature proofs.
The pedal's chain UI uses max-height, so that field remains present automatically.


The separate `css-ranges-v1` proof selects byte storage for nonnegative padding,
gap, border width/radius, font size, resolved line height and flex factors.
Numeric values and CSS pixel lengths have separate bounds; the CLI applies the
actual layout pixel ratio before choosing a type. Inheritance, native defaults,
unitless line heights and 255/256 boundaries are included. Missing or unknown
proof, dynamic styles, relative units, animation, runtime scale changes and
opaque native mutation retain 16-bit fields. These are generated build facts,
not application opt-ins. Grouped byte fields retain ordinary direct loads;
there is no pointer lookup, bitfield unpacking or per-read range check.

Source discovery uses TypeScript syntax trees for imports, exports and literal
module calls. A leading side-effect stylesheet import must not disappear when
followed by another import/export. Computed or malformed module boundaries
retain the conservative feature set. The pedal's 65px root radius is therefore
included in the range proof before its 2x layout scale is applied.


## Native node payload reachability

The independent `node-analysis-v1` fact set selects image nodes and native text
inputs from the complete discovered source graph. The CLI generates
`GEA_UI_IMAGE_NODES` and `GEA_UI_INPUT_NODES`; neither is an application opt-in.
Older analyzers, unresolved imports, dynamic element factories, opaque markup
and native UI mutation retain support. Literal tags are case-insensitive, as in
`Document::createElement`. CSS background images remain a separate renderer
feature, and custom button keyboards do not require native input/caret state.

An image-free build has no per-node image handle. An input-free build has no
focus/caret fields or native keyboard implementation. A scroll-free build omits
both scroll-dirty flags. Absent fields are compile-time constants; hot retained
style and layout fields remain embedded and directly readable. The native test
matrix exercises image/input support independently of each other and of CSS,
including positive image painting and input focus/caret behavior when enabled.

For the pedal, the linked S3 records are Node 160 B, ComputedStyle 88 B,
LayoutBox 38 B, RenderState 14 B and TreeState 29,816 B for 160 slots. Compared
with the preceding 164-byte candidate, this saves 648 B in TreeState and
11,664 B of firmware. The 49-node host allocation fixture is unchanged because
of allocator rounding; these record reductions are not a host heap saving.
This remains a development checkpoint, not a 50-byte node or a 60 fps guarantee.


## Inline SVG

`<svg>` and its shapes are ordinary nodes (`svg_element.h`): they take classes,
inline style, listeners and `:active`, and `getAttribute` returns full path
data. The `<svg>` box lays out like a view (its `width`/`height` attributes are
its default size; CSS wins) and records one `DrawSvg` display command. That
command rasterizes the svg's cached scene into the framebuffer with exact-area
anti-aliasing, honouring clip and opacity. Shapes are out of flow and zero-size.
The scene rebuilds once, on the next frame after anything in the subtree changes
(attribute, class, style, structure), and only the svg's box repaints.

Supported:

- Elements: `svg`, `g`, `path`, `polyline`, `polygon`, `line`, `circle`,
  `ellipse` and `rect` (with `rx`/`ry`). `defs`, `title`, `desc` and `text`
  are accepted and not painted.
- `viewBox` with `preserveAspectRatio` (all align values, `meet`/`slice`,
  `none`).
- Path data: `M L H V C S Q T A Z`, absolute and relative, with implicit
  repeats and compact arc flags. Parsing stops at the first error and keeps
  what came before it, as browsers do.
- `transform` on shapes and groups: `matrix`, `translate`, `scale`,
  `rotate(a [cx cy])`, `skewX` and `skewY`.
- Paint: `fill`, `stroke`, `stroke-width`, `stroke-linecap` (butt, round,
  square), `stroke-linejoin` (miter, round, bevel), `stroke-miterlimit`,
  `fill-rule`, `opacity`, `fill-opacity`, `stroke-opacity`, `color` with
  `currentColor`, `visibility`, `display` and `pointer-events`. Colours are
  hex (3, 4, 6 or 8 digits), `rgb[a]()`, `hsl[a]()`, named colours and
  `transparent`.
- The same paint properties as CSS (`.slice { fill: #38bdf8; stroke-width: 3px }`,
  `style={{ fill: color }}`). CSS beats presentation attributes, and the
  inheritable ones inherit through `g`. Group `opacity` multiplies into each
  shape; it is not composited as a layer.
- Touch: the hit test descends into the svg by geometry, either the fill's
  winding or the stroke within 3px of slop. The topmost shape receives the
  event, which bubbles through `g` and `svg`. `pointer-events: none` shapes
  are transparent to touch.

Not supported: `<text>`, `<use>`, gradients and patterns, `clipPath`/`mask`,
markers, dashes (`stroke-dasharray`), filters, and units other than px.
`var()` in paint values is not supported either. A CSS `transform` on the
`<svg>` element itself moves its box, but the scene is not reprojected into the
transformed space.

Cost: the svg paint fields live in the pooled `RareStyle`, allocated only for
nodes that set a rare field. Ordinary nodes carry nothing new. The plugin
reports `node-svg` when the source can create an svg element (JSX `<svg>`,
`createElement('svg')`, `SVG*Element`, opaque markup). The CLI turns that into
`GEA_UI_SVG`, which compiles the whole feature out of apps that have no svg.
