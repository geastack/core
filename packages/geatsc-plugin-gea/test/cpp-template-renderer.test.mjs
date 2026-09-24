import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  lowerRowSlots,
  lowerSlots,
  rootSetupLines,
} from "../dist/cpp-mounted-lowering.js";
import { applyTypedArrayStorage } from "../dist/cpp-store-typed-arrays.js";
import { diagnoseBoxedKeyedComponentProps } from "../dist/cpp-keyed-component-diagnostics.js";
import { mountedRendererForComponent } from "../dist/cpp-mounted.js";
import { inlineOnlyComponentClassesForIr } from "../dist/cpp-replacements.js";
import {
  readClassMethodBody,
  replaceClassMethodBody,
  replaceClassMethodParameters,
} from "../dist/cpp-store-method-targets.js";
import {
  collectStoreArraySources,
  collectStoreFields,
} from "../dist/cpp-stores.js";
import { templateMountedRenderer } from "../dist/cpp-template-renderer.js";
import { createGeaHostShims } from "../dist/host-shims.js";
import { generateCppIrNamespaceSource } from "../dist/cpp-ir.js";

test("native template content preserves the NodeHandle for firstChild lowering", () => {
  const shims = createGeaHostShims();
  const binding = shims.nativeMemberPropertyGetters.content.find((entry) =>
    entry.receiverTypes.includes("gea::embedded::ui::NodeHandle"),
  );

  assert.deepEqual(binding, {
    emit: "({receiver})",
    returnType: "gea::embedded::ui::NodeHandle",
    receiverTypes: shims.nativeMemberPropertyGetters.content[0].receiverTypes,
  });
});

test("tracked plain-value helper owns the exact embedded v1 value domain", () => {
  const source = generateCppIrNamespaceSource(
    {
      schema: "gea-ir",
      version: 1,
      entry: "/virtual.ts",
      modules: [],
      components: [],
      stores: [],
      hostCapabilities: [],
    },
    [{ sourceFile: { fileName: "/virtual.ts" }, relativePath: "virtual.cpp" }],
  );
  assert.match(
    source,
    /inline bool compiledStorePlainValue\(const gea_cpp_value &value\)/,
  );
  assert.match(source, /kind_t::proxy && candidate->rare_r\(\)\.proxy_target/);
  assert.match(
    source,
    /kind_t::array_value\) return candidate->rare_r\(\)\.typed_array_name\.empty\(\)/,
  );
  assert.match(
    source,
    /candidate->kind != gea_cpp_value::kind_t::record\) return false/,
  );
  assert.match(
    source,
    /prototype_names && !candidate->rare_r\(\)\.prototype_names->empty\(\)\) return false/,
  );
  assert.match(source, /prototype->kind == gea_cpp_value::kind_t::null_value/);
  assert.match(
    source,
    /gea_cpp_strict_equals\(\*prototype, gea_cpp_object_prototype\(\)\)/,
  );
});

test("store method replacement skips deferred declarations and targets their out-of-line definitions", () => {
  const header = `
class ReaderStore {
public:
  virtual void keydown(double keyCode) const;
  virtual std::string pageNumberLabel() const {
    return std::string("0 / 0");
  }
};
`;
  assert.equal(readClassMethodBody(header, "ReaderStore", "keydown"), null);
  assert.equal(
    replaceClassMethodBody(header, "ReaderStore", "keydown", [
      "{",
      "  replacement();",
      "}",
    ]),
    header,
  );
  assert.match(
    readClassMethodBody(header, "ReaderStore", "pageNumberLabel"),
    /0 \/ 0/,
  );

  const source = `
void ReaderStore::keydown(double keyCode) const {
  original(keyCode);
}
`;
  assert.match(
    readClassMethodBody(source, "ReaderStore", "keydown"),
    /original\(keyCode\)/,
  );
  const replaced = replaceClassMethodBody(source, "ReaderStore", "keydown", [
    "{",
    "  replacement(keyCode);",
    "}",
  ]);
  assert.match(
    replaced,
    /ReaderStore::keydown\(double keyCode\) const \{\n  replacement\(keyCode\);\n\}/,
  );
  assert.doesNotMatch(replaced, /original\(keyCode\)/);
});

test("typed store parameter lowering preserves deferred method signatures", () => {
  const source = `
void ReaderStore::keydown(const auto & keyCode) const {
  original(keyCode);
}
`;
  const replaced = replaceClassMethodParameters(
    source,
    "ReaderStore",
    "keydown",
    "double keyCode",
    ["{", "  replacement(keyCode);", "}"],
  );
  assert.match(
    replaced,
    /ReaderStore::keydown\(const auto & keyCode\) const \{/,
  );
  assert.match(replaced, /replacement\(keyCode\)/);
  assert.doesNotMatch(replaced, /keydown\(double keyCode\)/);
});

test("primitive store getters lower to typed calls with their field dependencies", () => {
  const work = mkdtempSync(join(tmpdir(), "gea-store-getter-"));
  const module = join(work, "reader-store.ts");
  writeFileSync(
    module,
    `
class ReaderStore {
  get readerClass(): string { return '' }
  get pageNumberLabel(): string { return '' }
  get progressPercent(): number { return 0 }
  get progressHeight(): number { return 0 }
}
export const reader = new ReaderStore()
`,
  );
  try {
    const storeFields = collectStoreFields([
      {
        id: "ReaderStore",
        module,
        className: "ReaderStore",
        runtimeBase: "compiled",
        fields: [
          {
            name: "fontIndex",
            shape: { kind: "literal", valueType: "number" },
          },
          {
            name: "pageIndex",
            shape: { kind: "literal", valueType: "number" },
          },
          {
            name: "pages",
            shape: {
              kind: "array",
              element: { kind: "literal", valueType: "string" },
            },
          },
        ],
        getters: [
          {
            name: "readerClass",
            returnsArray: false,
            deps: ["fontIndex"],
            body: "{ return 'reader-screen font-' + this.fontIndex }",
          },
          {
            name: "pageNumberLabel",
            returnsArray: false,
            deps: ["pages", "pageIndex"],
            body: "{ return this.pageIndex + ' / ' + this.pages.length }",
          },
          {
            name: "progressPercent",
            returnsArray: false,
            deps: ["pages", "pageIndex"],
            body: "{ return 0 }",
          },
          {
            name: "progressHeight",
            returnsArray: false,
            deps: ["progressPercent"],
            body: "{ return this.progressPercent }",
          },
        ],
      },
    ]);
    assert.ok(
      storeFields.some((field) => field.fieldName === "readerClass"),
      JSON.stringify(storeFields, null, 2),
    );
    const component = {
      id: "ReaderScreen",
      module: "reader-screen.tsx",
      exportName: "ReaderScreen",
      runtimeBase: "compiled",
      template: {
        html: "<div><span>0</span></div>",
        slots: [
          { index: 0, kind: "class", walk: [], expr: "reader.readerClass" },
          {
            index: 1,
            kind: "text",
            walk: [0, 0],
            expr: "reader.pageNumberLabel",
          },
          {
            index: 2,
            kind: "text",
            walk: [0, 0],
            expr: "reader.progressHeight",
          },
        ],
      },
    };

    const lines = templateMountedRenderer(
      component,
      [component],
      storeFields,
      new Map(),
      () => true,
    );
    assert.ok(lines, "expected the getter-backed component to lower");
    const source = lines.join("\n");
    assert.match(source, /__gea_store->readerClass\(\)/);
    assert.match(source, /__gea_store->pageNumberLabel\(\)/);
    assert.match(source, /__gea_store->progressHeight\(\)/);
    assert.match(source, /\{"fontIndex"\}/);
    assert.match(source, /\{"pages", "pageIndex"\}/);
    assert.doesNotMatch(source, /record_get_literal\("readerClass"\)/);
    assert.doesNotMatch(source, /record_get_literal\("pageNumberLabel"\)/);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
});

function renderClassExpression(expr) {
  const component = {
    id: "Cell",
    module: "test",
    exportName: "Cell",
    runtimeBase: "compiled",
    template: {
      html: "<div>0</div>",
      slots: [
        { index: 0, kind: "class", walk: [], expr },
        { index: 1, kind: "text", walk: [0], expr: "game.cell0" },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "cell0",
      fieldType: "gea_cpp_value",
      readerName: "read_game_cell0",
      field: { name: "cell0" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(lines, "expected the template renderer to lower the component");
  return lines.join("\n");
}

test("dynamic template equality uses runtime equality helpers in class bindings", () => {
  const cases = [
    {
      expr: "game.cell0 == 'O' ? 'tic-cell tic-cell-o' : 'tic-cell tic-cell-x'",
      helper:
        'gea_cpp_loose_equals(gea_cpp_key(__gea_cell0), gea_cpp_key(std::string("O")))',
    },
    {
      expr: "game.cell0 != 'O' ? 'tic-cell tic-cell-x' : 'tic-cell tic-cell-o'",
      helper:
        '(!gea_cpp_loose_equals(gea_cpp_key(__gea_cell0), gea_cpp_key(std::string("O"))))',
    },
    {
      expr: "game.cell0 === 'O' ? 'tic-cell tic-cell-o' : 'tic-cell tic-cell-x'",
      helper:
        'gea_cpp_strict_equals(gea_cpp_key(__gea_cell0), gea_cpp_key(std::string("O")))',
    },
    {
      expr: "game.cell0 !== 'O' ? 'tic-cell tic-cell-x' : 'tic-cell tic-cell-o'",
      helper:
        '(!gea_cpp_strict_equals(gea_cpp_key(__gea_cell0), gea_cpp_key(std::string("O"))))',
    },
  ];

  for (const { expr, helper } of cases) {
    const source = renderClassExpression(expr);
    assert.match(
      source,
      new RegExp(helper.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")),
    );
    assert.doesNotMatch(
      source,
      /gea_cpp_key\(__gea_cell0\)\s*[!=]==?\s*std::string\("O"\)/,
    );
    assert.doesNotMatch(source, /__gea_cell0\s*[!=]==?\s*std::string\("O"\)/);
  }
});

// Typed store field (literal string shape) — the comparison MUST lower natively,
// never re-box a typed std::string through gea_cpp_value/gea_cpp_key just to run
// `==`. This is the case the real GameStore (std::string cells) hits.
function renderTypedClassExpression(expr) {
  const component = {
    id: "Cell",
    module: "test",
    exportName: "Cell",
    runtimeBase: "compiled",
    template: {
      html: "<div>0</div>",
      slots: [
        { index: 0, kind: "class", walk: [], expr },
        { index: 1, kind: "text", walk: [0], expr: "game.cell0" },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "cell0",
      fieldType: "std::string",
      readerName: "read_game_cell0",
      field: { name: "cell0" },
      shape: { kind: "literal", valueType: "string" },
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(
    lines,
    "expected the template renderer to lower the typed component",
  );
  return lines.join("\n");
}

test("typed string-field equality lowers to NATIVE comparison (no gea_cpp_value boxing)", () => {
  const cases = [
    {
      expr: "game.cell0 == 'O' ? 'a' : 'b'",
      native: '(__gea_cell0 == std::string("O"))',
    },
    {
      expr: "game.cell0 === 'O' ? 'a' : 'b'",
      native: '(__gea_cell0 == std::string("O"))',
    },
    {
      expr: "game.cell0 != 'O' ? 'a' : 'b'",
      native: '(__gea_cell0 != std::string("O"))',
    },
    {
      expr: "game.cell0 !== 'O' ? 'a' : 'b'",
      native: '(__gea_cell0 != std::string("O"))',
    },
  ];

  for (const { expr, native } of cases) {
    const source = renderTypedClassExpression(expr);
    assert.match(
      source,
      new RegExp(native.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")),
    );
    // a typed std::string comparison must NOT box through gea_cpp_key / dynamic equals
    assert.doesNotMatch(source, /gea_cpp_key\(__gea_cell0\)/);
    assert.doesNotMatch(source, /gea_cpp_(loose|strict)_equals\([^)]*cell0/);
  }
});

test("primitive fieldType preserves native template comparisons when shape is missing", () => {
  const component = {
    id: "Cell",
    module: "test",
    exportName: "Cell",
    runtimeBase: "compiled",
    template: {
      html: "<div>0</div>",
      slots: [
        {
          index: 0,
          kind: "class",
          walk: [],
          expr: "game.cell0 === 'O' ? 'a' : 'b'",
        },
        { index: 1, kind: "text", walk: [0], expr: "game.cell0" },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "cell0",
      fieldType: "std::string",
      readerName: "read_game_cell0",
      field: { name: "cell0" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(
    lines,
    "expected the template renderer to lower the fieldType-only component",
  );
  const source = lines.join("\n");
  assert.match(source, /\(__gea_cell0 == std::string\("O"\)\)/);
  assert.doesNotMatch(source, /gea_cpp_key\(__gea_cell0\)/);
  assert.doesNotMatch(source, /gea_cpp_(loose|strict)_equals\([^)]*cell0/);
});

test("typed array storage strips boxed direct store-field push records", () => {
  const source = `
struct __gea_record_7 {
  double v;
  bool __gea_has_v;
  gea_cpp_value __gea_to_value() const { gea_cpp_value out; return out; }
};
inline gea_cpp_value gea_cpp_key(const __gea_record_7 &__gea_v) {
  gea_cpp_value out;
  return out;
}
struct __gea_record_8 {
  double x;
  bool __gea_has_x;
  gea_cpp_value __gea_to_value() const { gea_cpp_value out; return out; }
};
inline gea_cpp_value gea_cpp_key(const __gea_record_8 &__gea_v) {
  gea_cpp_value out;
  return out;
}
class GameStore {
public:
  mutable std::vector<gea_cpp_value> solid{};
  GameStore() {
    this->solid = std::vector<gea_cpp_value>{([&]() { gea_cpp_value __gea_record_value_98; __gea_record_value_98.kind = gea_cpp_value::kind_t::record; __gea_record_value_98.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>(); __gea_record_value_98.record_set_literal("v", 0); return __gea_record_value_98; })()};
  }
  virtual void populateLevel() const {
    (*this).solid.push_back(gea_cpp_key(([&]() { return __gea_record_7{.v = x, .__gea_has_v = true}; })()));
  }
};
`;
  const ir = {
    stores: [
      {
        className: "GameStore",
        fields: [
          {
            name: "solid",
            typedStorage: {
              itemType: "GameStore_solid_item",
              readerName: "read_GameStore_field_solid",
              useInterfaceStruct: false,
            },
            shape: {
              kind: "array",
              element: {
                kind: "object",
                fields: [
                  {
                    name: "v",
                    shape: { kind: "literal", valueType: "number" },
                  },
                ],
              },
            },
          },
        ],
      },
    ],
  };

  const rewritten = applyTypedArrayStorage(source, ir);
  assert.match(
    rewritten,
    /mutable std::vector<gea_ir::GameStore_solid_item> solid\{\};/,
  );
  assert.match(
    rewritten,
    /this->solid = std::vector<gea_ir::GameStore_solid_item>\{gea_ir::GameStore_solid_item\(static_cast<double>\(0\)\)\};/,
  );
  assert.match(
    rewritten,
    /\(\*this\)\.solid\.push_back\(gea_ir::GameStore_solid_item\(static_cast<double>\(x\)\)\);/,
  );
  assert.doesNotMatch(rewritten, /__gea_record_7/);
  assert.doesNotMatch(rewritten, /__gea_record_8/);
  assert.doesNotMatch(rewritten, /std::vector<gea_cpp_value> __gea_seed_solid/);
  assert.doesNotMatch(rewritten, /solid\.push_back\(gea_cpp_key/);
});

test("primitive fieldType preserves native mounted style applies when shape is missing", () => {
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "width",
      fieldType: "double",
      readerName: "read_game_width",
      field: { name: "width" },
      shape: null,
      reader: null,
    },
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "visible",
      fieldType: "bool",
      readerName: "read_game_visible",
      field: { name: "visible" },
      shape: null,
      reader: null,
    },
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "color",
      fieldType: "std::string",
      readerName: "read_game_color",
      field: { name: "color" },
      shape: null,
      reader: null,
    },
  ];
  const lowered = lowerSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "width", expr: "game.width", exprPath: ["game", "width"] },
          { name: "height", expr: "`${game.width}px`" },
          { name: "left", expr: "`${game.width}%`" },
          { name: "translateX", expr: "`${game.width}px`" },
          { name: "transform", expr: "`rotate(${game.width}deg)`" },
          { name: "rotate", expr: "`${game.width}deg`" },
          { name: "right", expr: "game.width", exprPath: ["game", "width"] },
          { name: "bottom", expr: "game.width", exprPath: ["game", "width"] },
          { name: "padding", expr: "game.width", exprPath: ["game", "width"] },
          {
            name: "marginTop",
            expr: "game.width",
            exprPath: ["game", "width"],
          },
          {
            name: "borderRadius",
            expr: "game.width",
            exprPath: ["game", "width"],
          },
          {
            name: "borderWidth",
            expr: "game.width",
            exprPath: ["game", "width"],
          },
          { name: "fontSize", expr: "game.width", exprPath: ["game", "width"] },
          { name: "zIndex", expr: "game.width", exprPath: ["game", "width"] },
          { name: "flex", expr: "game.width", exprPath: ["game", "width"] },
          {
            name: "opacity",
            expr: "game.visible",
            exprPath: ["game", "visible"],
          },
          { name: "color", expr: "game.color", exprPath: ["game", "color"] },
        ],
      },
    ],
    storeFields,
  );

  assert.ok(lowered, "expected mounted style slots to lower");
  const source = lowered.lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Width, .*static_cast<double>\(__gea_width\)/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Height, .*static_cast<double>\(__gea_width\)/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::LeftPercent, static_cast<int>\(std::round\(static_cast<double>\(__gea_width\) \* 10\.0\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.translateX\(.*static_cast<double>\(__gea_width\)/,
  );
  assert.match(source, /root\.style\(\)\.cssRotateDegrees\(/);
  assert.equal(
    [
      ...source.matchAll(
        /root\.style\(\)\.(?:rotateDegrees|cssRotateDegrees)\(static_cast<double>\(__gea_width\)\);/g,
      ),
    ].length,
    2,
  );
  for (const property of [
    "Right",
    "Bottom",
    "PaddingTop",
    "PaddingRight",
    "PaddingBottom",
    "PaddingLeft",
    "MarginTop",
    "BorderRadiusTopLeft",
    "BorderRadiusTopRight",
    "BorderRadiusBottomRight",
    "BorderRadiusBottomLeft",
    "FontSize",
    "ZIndex",
    "Flex",
  ]) {
    assert.match(
      source,
      new RegExp(
        `root\\.style\\(\\)\\.set\\(gea::embedded::ui::Property::${property}, .*static_cast<double>\\(__gea_width\\)`,
      ),
    );
  }
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::FlexBasis, gea::embedded::ui::kUnset\);/,
  );
  assert.match(
    source,
    /applyProperty\(root, gea::embedded::ui::StyleDeclaration::Opacity, gea_cpp_to_string\(__gea_visible\)\);/,
  );
  assert.match(
    source,
    /applyProperty\(root, gea::embedded::ui::StyleDeclaration::Color, __gea_color\);/,
  );
  assert.match(source, /applyNumberProperty\(root, gea::embedded::ui::StyleDeclaration::BorderWidth, static_cast<double>\(__gea_width\)\)/);
  assert.doesNotMatch(source, /gea_cpp_key\(__gea_(?:width|visible|color)\)/);
});

test("template style rotate degrees lowers to direct numeric apply", () => {
  const component = {
    id: "Needle",
    module: "test",
    exportName: "Needle",
    runtimeBase: "compiled",
    template: {
      html: "<div><span></span></div>",
      slots: [
        {
          index: 0,
          kind: "style",
          walk: [0],
          exprObjectFields: [
            { name: "transform", expr: "`rotate(${game.angle}deg)`" },
            { name: "rotate", expr: "`${game.angle}deg`" },
            { name: "scale", expr: "game.angle" },
            { name: "width", expr: "`${game.angle}%`" },
            { name: "padding", expr: "game.angle" },
            { name: "zIndex", expr: "game.angle" },
            { name: "opacity", expr: "game.angle" },
            { name: "flex", expr: "game.angle" },
          ],
        },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "angle",
      fieldType: "double",
      readerName: "read_game_angle",
      field: { name: "angle" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(lines, "expected the template renderer to lower rotate styles");
  const source = lines.join("\n");
  assert.match(source, /\.style\(\)\.cssRotateDegrees\(static_cast<double>\(__gea_angle\)\);/);
  assert.match(source, /\.style\(\)\.cssScale\(static_cast<double>\(__gea_angle\)\);/);
  assert.equal(
    [
      ...source.matchAll(
        /\.style\(\)\.(?:rotateDegrees|cssRotateDegrees)\(static_cast<double>\(__gea_angle\)\);/g,
      ),
    ].length,
    2,
  );
  assert.match(
    source,
    /\.style\(\)\.set\(gea::embedded::ui::Property::WidthPercent, static_cast<int>\(std::round\(static_cast<double>\(__gea_angle\) \* 10\.0\)\)\);/,
  );
  for (const property of [
    "PaddingTop",
    "PaddingRight",
    "PaddingBottom",
    "PaddingLeft",
    "ZIndex",
    "Flex",
  ]) {
    assert.match(
      source,
      new RegExp(
        `\\.style\\(\\)\\.set\\(gea::embedded::ui::Property::${property}, .*static_cast<double>\\(__gea_angle\\)`,
      ),
    );
  }
  assert.match(
    source,
    /\.style\(\)\.opacity\(.*static_cast<double>\(__gea_angle\)/,
  );
  assert.match(
    source,
    /\.style\(\)\.set\(gea::embedded::ui::Property::FlexBasis, gea::embedded::ui::kUnset\);/,
  );
  assert.doesNotMatch(source, /StyleDeclaration::(?:Transform|Rotate)/);
  assert.doesNotMatch(
    source,
    /applyNumberProperty\([^,]+, gea::embedded::ui::StyleDeclaration::(?:Padding|ZIndex|Opacity|Flex)/,
  );
});

test("literal keyword inline styles lower to direct property writes in template renderer", () => {
  const component = {
    id: "Panel",
    module: "test",
    exportName: "Panel",
    runtimeBase: "compiled",
    template: {
      html: "<div></div>",
      slots: [
        {
          index: 0,
          kind: "style",
          walk: [],
          exprObjectFields: [
            { name: "display", expr: "'flex'" },
            { name: "position", expr: "'absolute'" },
            { name: "alignItems", expr: "'center'" },
            { name: "alignContent", expr: "'flex-start'" },
            { name: "alignSelf", expr: "'stretch'" },
          ],
        },
      ],
    },
  };
  const lines = templateMountedRenderer(
    component,
    [component],
    [],
    new Map(),
    () => true,
  );
  assert.ok(
    lines,
    "expected the template renderer to lower literal keyword styles",
  );
  const source = lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, 3\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Position, 1\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::AlignItems, 1\);/,
  );
  assert.match(source, /Property::AlignContent, 6\);/);
  assert.match(source, /Property::AlignSelf, 0\);/);
  assert.doesNotMatch(
    source,
    /applyProperty\(root, "(?:display|position|align-items)"/,
  );
});

test("white-space keyword styles preserve distinct collapse and wrapping modes", () => {
  for (const [keyword, value] of [['normal', 0], ['nowrap', 1], ['pre', 2], ['pre-wrap', 3], ['pre-line', 4], ['break-spaces', 5]]) {
    const source = rootSetupLines("root", [{
      index: 0, kind: "style", walk: [], exprObjectFields: [{ name: 'whiteSpace', expr: `'${keyword}'` }],
    }]).join('\n');
    assert.ok(source.includes(`Property::WhiteSpace, ${value});`), `${keyword}: ${source}`);
  }
});

test("layout dependency styles reach the native property parser", () => {
  for (const [name, expr, declaration] of [
    ["boxSizing", "'border-box'", "BoxSizing"],
    ["float", "'left'", "Float"],
    ["clear", "'both'", "Clear"],
    ["marginTrim", "'block-start block-end'", "MarginTrim"],
    ["writingMode", "'vertical-lr'", "WritingMode"],
    ["direction", "'rtl'", "Direction"],
    ["flexFlow", "'row wrap'", "FlexFlow"],
    ["rowGap", "'10%'", "RowGap"],
    ["columnGap", "'20px'", "ColumnGap"],
    ["backgroundPosition", "'50% 10px'", "BackgroundPosition"],
    ["backgroundRepeat", "'no-repeat'", "BackgroundRepeat"],
    ["backgroundAttachment", "'fixed'", "BackgroundAttachment"],
    ["backgroundOrigin", "'content-box'", "BackgroundOrigin"],
    ["font", "'20px/1 Ahem'", "Font"],
    ["width", "'5ch'", "Width"],
    ["height", "'calc(2ch + 1px)'", "Height"],
  ]) {
    const source = rootSetupLines("root", [{
      index: 0, kind: "style", walk: [], exprObjectFields: [{ name, expr }],
    }]).join("\n");
    const property = name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`);
    assert.ok(source.includes(`StyleDeclaration::${declaration}`) ||
      source.includes(`applyProperty(root, "${property}"`), source);
    assert.doesNotMatch(source, /StyleDeclaration::Ignored/);
  }
  const source = rootSetupLines("root", [{
    index: 0, kind: "style", walk: [],
    exprObjectFields: [{ name: "textAlign", expr: "'start'" }],
  }]).join("\n");
  assert.match(source, /Property::TextAlign, 0\);/);
});

test("fractional border widths retain numeric precision and CSS px units", () => {
  const fields = [{
    storeClass: "GameStore", stateType: "GameStore", storeGlobalName: "game",
    fieldName: "width", fieldType: "double", readerName: "read_game_width",
    field: { name: "width" }, shape: null, reader: null,
  }];
  for (const name of ["borderWidth", "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth"]) {
    const declaration = name[0].toUpperCase() + name.slice(1);
    for (const expr of ["1.9", "game.width", "`${game.width}px`", "'1.9px'"]) {
      const slot = { index: 0, kind: "style", walk: [], exprObjectFields: [{ name, expr, ...(expr === "game.width" ? { exprPath: ["game", "width"] } : {}) }] };
      const component = { id: "Border", module: "test", exportName: "Border", runtimeBase: "compiled",
        template: { html: "<div></div>", slots: [slot] } };
      const outputs = [
        expr.includes("game.") ? lowerSlots([slot], fields)?.lines.join("\n") : rootSetupLines("root", [slot]).join("\n"),
        templateMountedRenderer(component, [component], fields, new Map(), () => true)?.join("\n"),
      ];
      for (const source of outputs) {
        assert.ok(source, `${name}: ${expr} must lower`);
        assert.ok(source.includes(`StyleDeclaration::${declaration}`) || source.includes(`"${name.replace(/[A-Z]/g, c => `-${c.toLowerCase()}`)}"`), source);
        assert.doesNotMatch(source, new RegExp(`Property::${declaration}, .*std::round`));
        assert.doesNotMatch(source, /gea_cpp_key\(__gea_width\)/);
        if (expr.startsWith("`")) {
          assert.match(source, /applyPixelLengthProperty\(/);
          assert.doesNotMatch(source, /gea_cpp_(?:to_string|key)\(__gea_width\)/);
        } else if (expr.includes("px")) {
          assert.match(source, /applyProperty\(/);
          assert.match(source, /px/);
          assert.doesNotMatch(source, /applyNumberProperty\(/);
        } else {
          assert.match(source, /applyNumberProperty\(/);
        }
      }
    }
  }
});

test("order styles use the validated numeric declaration path", () => {
  const lines = rootSetupLines("root", [{
    index: 0,
    kind: "style",
    walk: [],
    exprObjectFields: [{ name: "order", expr: "-2" }],
  }]);
  assert.match(lines.join("\n"), /StyleDeclaration::Order/);
});

test("literal keyword root setup styles lower to direct property writes", () => {
  const lines = rootSetupLines("root", [
    {
      index: 0,
      kind: "style",
      walk: [],
      exprObjectFields: [
        { name: "display", expr: "'grid'" },
        { name: "placeItems", expr: "'center'" },
        { name: "textAlign", expr: "'right'" },
      ],
    },
  ]);
  const source = lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, 2\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::AlignItems, 1\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::JustifyItems, 1\);/,
  );
  assert.doesNotMatch(source, /Property::JustifyContent/);
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::TextAlign, 2\);/,
  );
  assert.doesNotMatch(
    source,
    /applyProperty\(root, "(?:display|place-items|text-align)"/,
  );
});


test("compound alignment styles preserve the native shorthand parser", () => {
  const source = rootSetupLines("root", [{
    index: 0, kind: "style", walk: [],
    exprObjectFields: [
      { name: "placeItems", expr: "'safe center unsafe end'" },
      { name: "placeContent", expr: "'end space-evenly'" },
      { name: "placeSelf", expr: "'first baseline center'" },
    ],
  }]).join("\n");
  assert.match(source, /applyProperty\(root, "place-items", std::string\("safe center unsafe end"\)\)/);
  assert.match(source, /applyProperty\(root, "place-content", std::string\("end space-evenly"\)\)/);
  assert.match(source, /applyProperty\(root, "place-self", std::string\("first baseline center"\)\)/);
  assert.doesNotMatch(source, /gea::Value/);
});

test("constant root setup styles lower to direct property writes", () => {
  const constants = new Map([
    ["ROOT_WIDTH", { valueType: "number", value: "42" }],
    ["ROOT_COLOR", { valueType: "string", value: "#123456" }],
    ["ROOT_DISPLAY", { valueType: "string", value: "grid" }],
  ]);
  const lines = rootSetupLines(
    "root",
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "width", expr: "ROOT_WIDTH" },
          { name: "color", expr: "ROOT_COLOR" },
          { name: "display", expr: "ROOT_DISPLAY" },
        ],
      },
    ],
    [],
    [],
    undefined,
    undefined,
    constants,
  );
  const source = lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Width, .*static_cast<double>\(42\)/,
  );
  assert.match(
    source,
    /root\.style\(\)\.color\(static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(18, 52, 86\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, 2\);/,
  );
  assert.doesNotMatch(source, /apply(?:Number)?Property\(root/);
});

test("delegated static events unregister their exact body listener through the branch disposer", () => {
  const lines = rootSetupLines(
    "cityButton",
    [
      {
        index: 7,
        kind: "event",
        walk: [],
        expr: "() => closeManager()",
        payload: { attrName: "onClick" },
      },
    ],
    [],
    [],
    undefined,
    "button",
    new Map(),
    "__gea_branch_disposer",
  );
  assert.ok(lines, "expected the delegated event to lower");
  const source = lines.join("\n");
  assert.match(
    source,
    /auto __gea_event_listener_cityButton_7 = .*body\(\)\.addEventListener\("click"/,
  );
  assert.match(source, /containsNode\(cityButton\.id\(\), event\.targetId\)/);
  assert.match(
    source,
    /__gea_branch_disposer->add\(\[__gea_event_listener_cityButton_7\]/,
  );
  assert.match(
    source,
    /body\(\)\.removeEventListener\("click", __gea_event_listener_cityButton_7\)/,
  );
  assert.doesNotMatch(source, /cityButton\.addEventListener/);
});

test("mixed mounted root style constants stay outside reactive applies", () => {
  const constants = new Map([
    ["ROOT_COLOR", { valueType: "string", value: "#123456" }],
    ["ROOT_DISPLAY", { valueType: "string", value: "grid" }],
  ]);
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "width",
      fieldType: "double",
      readerName: "read_game_width",
      field: { name: "width" },
      shape: null,
      reader: null,
    },
  ];
  const lowered = lowerSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "width", expr: "game.width", exprPath: ["game", "width"] },
          { name: "color", expr: "ROOT_COLOR" },
          { name: "display", expr: "ROOT_DISPLAY" },
        ],
      },
    ],
    storeFields,
    constants,
  );
  assert.ok(
    lowered,
    "expected mixed static/dynamic mounted style slot to lower",
  );
  const staticSource = lowered.staticLines.join("\n");
  const dynamicSource = lowered.lines.join("\n");
  assert.match(
    staticSource,
    /root\.style\(\)\.color\(static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(18, 52, 86\)\)\);/,
  );
  assert.match(
    staticSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, 2\);/,
  );
  assert.match(
    dynamicSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Width, .*static_cast<double>\(__gea_width\)/,
  );
  assert.doesNotMatch(
    dynamicSource,
    /(?:Color|Display|ROOT_COLOR|ROOT_DISPLAY)/,
  );
  assert.doesNotMatch(staticSource, /__gea_width|apply(?:Number)?Property/);
});

test("dynamic keyword inline styles lower to direct property writes in template renderer", () => {
  const component = {
    id: "Panel",
    module: "test",
    exportName: "Panel",
    runtimeBase: "compiled",
    template: {
      html: "<div></div>",
      slots: [
        {
          index: 0,
          kind: "style",
          walk: [],
          exprObjectFields: [
            { name: "display", expr: "game.active ? 'flex' : 'none'" },
            { name: "placeItems", expr: "game.active ? 'center' : 'end'" },
            {
              name: "backfaceVisibility",
              expr: "game.active ? 'hidden' : 'visible'",
            },
            { name: "overflow", expr: "game.active ? 'hidden' : 'visible'" },
          ],
        },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "active",
      fieldType: "bool",
      readerName: "read_game_active",
      field: { name: "active" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(
    lines,
    "expected the template renderer to lower dynamic keyword styles",
  );
  const source = lines.join("\n");
  assert.equal([...source.matchAll(/bindReactiveApply/g)].length, 1);
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, static_cast<int>\(\(__gea_active \? 3 : 1\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::AlignItems, static_cast<int>\(\(__gea_active \? 1 : 8\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::JustifyItems, static_cast<int>\(\(__gea_active \? 1 : 8\)\)\);/,
  );
  assert.doesNotMatch(source, /Property::JustifyContent/);
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Backface, static_cast<int>\(\(__gea_active \? 1 : 0\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Overflow, static_cast<int>\(\(__gea_active \? 1 : 0\)\)\);/,
  );
  assert.doesNotMatch(
    source,
    /applyProperty\(root, gea::embedded::ui::StyleDeclaration::(?:Display|PlaceItems|Backface|Overflow)/,
  );
  assert.doesNotMatch(
    source,
    /gea_cpp_to_string\(__gea_active \? std::string\("flex"\) : std::string\("none"\)\)/,
  );
});

test("mounted root and row dynamic keyword styles lower to direct property writes", () => {
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "active",
      fieldType: "bool",
      readerName: "read_game_active",
      field: { name: "active" },
      shape: null,
      reader: null,
    },
  ];
  const lowered = lowerSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          {
            name: "display",
            expr: "game.active ? 'flex' : 'none'",
            exprPath: ["game", "active"],
          },
          {
            name: "placeItems",
            expr: "game.active ? 'center' : 'end'",
            exprPath: ["game", "active"],
          },
        ],
      },
    ],
    storeFields,
  );
  assert.ok(lowered, "expected mounted root keyword styles to lower");
  const rootSource = lowered.lines.join("\n");
  assert.match(
    rootSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, static_cast<int>\(\(__gea_active \? 3 : 1\)\)\);/,
  );
  assert.match(
    rootSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::AlignItems, static_cast<int>\(\(__gea_active \? 1 : 8\)\)\);/,
  );
  assert.match(
    rootSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::JustifyItems, static_cast<int>\(\(__gea_active \? 1 : 8\)\)\);/,
  );
  assert.doesNotMatch(rootSource, /Property::JustifyContent/);
  assert.doesNotMatch(rootSource, /StyleDeclaration::(?:Display|PlaceItems)/);
  assert.doesNotMatch(rootSource, /applyProperty\(root/);

  const field = {
    storeClass: "GameStore",
    stateType: "GameStore",
    fieldName: "rows",
    fieldType: "std::vector<GameStore_rows_item>",
    readerName: "read_game_rows",
    itemType: "GameStore_rows_item",
    itemTypeRef: "GameStore_rows_item",
    useInterfaceStruct: false,
    itemFields: [
      { name: "visible", shape: { kind: "literal", valueType: "boolean" } },
    ],
  };
  const rows = lowerRowSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "display", expr: "item.visible ? 'flex' : 'none'" },
          { name: "placeItems", expr: "item.visible ? 'center' : 'end'" },
        ],
      },
    ],
    "item",
    field,
  );
  const rowSource = rows.lines.join("\n");
  assert.match(
    rowSource,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::Display, static_cast<int>\(\(item\.visible \? 3 : 1\)\)\);/,
  );
  assert.match(
    rowSource,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::AlignItems, static_cast<int>\(\(item\.visible \? 1 : 8\)\)\);/,
  );
  assert.match(
    rowSource,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::JustifyItems, static_cast<int>\(\(item\.visible \? 1 : 8\)\)\);/,
  );
  assert.doesNotMatch(rowSource, /Property::JustifyContent/);
  assert.doesNotMatch(rowSource, /StyleDeclaration::(?:Display|PlaceItems)/);
  assert.doesNotMatch(rowSource, /applyProperty\(row/);
});

test("string constants in inline styles lower without runtime CSS parsing", () => {
  const constants = new Map([
    ["COLOR_TEXT", { valueType: "string", value: "#123456" }],
    ["COLOR_A", { valueType: "string", value: "#ffffff" }],
    ["COLOR_B", { valueType: "string", value: "#000000" }],
    ["DISPLAY_MODE", { valueType: "string", value: "flex" }],
    ["DISPLAY_OFF", { valueType: "string", value: "none" }],
  ]);
  const component = {
    id: "Panel",
    module: "test",
    exportName: "Panel",
    runtimeBase: "compiled",
    template: {
      html: "<div></div>",
      slots: [
        {
          index: 0,
          kind: "style",
          walk: [],
          exprObjectFields: [
            { name: "color", expr: "COLOR_TEXT" },
            { name: "display", expr: "DISPLAY_MODE" },
            {
              name: "backgroundColor",
              expr: "game.active ? COLOR_A : COLOR_B",
            },
          ],
        },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "active",
      fieldType: "bool",
      readerName: "read_game_active",
      field: { name: "active" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    constants,
    () => true,
  );
  assert.ok(lines, "expected the template renderer to lower constant styles");
  const source = lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.color\(static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(18, 52, 86\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, 3\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.backgroundColor\(static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 255, 255\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\)\);/,
  );
  assert.equal([...source.matchAll(/bindReactiveApply/g)].length, 1);
  const firstBind = source.indexOf("bindReactiveApply");
  assert.ok(
    source.indexOf("Property::Display") < firstBind,
    "constant keyword style should be applied before the reactive binding",
  );
  assert.ok(
    source.indexOf("style().color") < firstBind,
    "constant color style should be applied before the reactive binding",
  );
  assert.doesNotMatch(
    source,
    /applyProperty\(root, gea::embedded::ui::StyleDeclaration::(?:Color|Display|Background)/,
  );

  const lowered = lowerSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          {
            name: "backgroundColor",
            expr: "game.active ? COLOR_A : COLOR_B",
            exprPath: ["game", "active"],
          },
          {
            name: "display",
            expr: "game.active ? DISPLAY_MODE : DISPLAY_OFF",
            exprPath: ["game", "active"],
          },
        ],
      },
    ],
    storeFields,
    constants,
  );
  assert.ok(lowered, "expected mounted root constant-branch styles to lower");
  const rootSource = lowered.lines.join("\n");
  assert.match(
    rootSource,
    /root\.style\(\)\.backgroundColor\(static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 255, 255\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\)\);/,
  );
  assert.match(
    rootSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::Display, static_cast<int>\(\(__gea_active \? 3 : 1\)\)\);/,
  );
  assert.doesNotMatch(rootSource, /applyProperty\(root/);

  const field = {
    storeClass: "GameStore",
    stateType: "GameStore",
    fieldName: "rows",
    fieldType: "std::vector<GameStore_rows_item>",
    readerName: "read_game_rows",
    itemType: "GameStore_rows_item",
    itemTypeRef: "GameStore_rows_item",
    useInterfaceStruct: false,
    itemFields: [
      { name: "visible", shape: { kind: "literal", valueType: "boolean" } },
    ],
  };
  const rows = lowerRowSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "color", expr: "COLOR_TEXT" },
          {
            name: "display",
            expr: "item.visible ? DISPLAY_MODE : DISPLAY_OFF",
          },
        ],
      },
    ],
    "item",
    field,
    constants,
  );
  const rowSource = rows.lines.join("\n");
  assert.match(
    rowSource,
    /row\.style\(\)\.color\(static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(18, 52, 86\)\)\);/,
  );
  assert.match(
    rowSource,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::Display, static_cast<int>\(\(item\.visible \? 3 : 1\)\)\);/,
  );
  assert.doesNotMatch(rowSource, /applyProperty\(row/);
});

test("background style shorthand resets images while the color longhand preserves them", () => {
  for (const property of ["background", "backgroundColor", "backgroundImage"]) {
    const component = {
      id: "BackgroundPanel", module: "test", exportName: "BackgroundPanel", runtimeBase: "compiled",
      template: { html: "<div></div>", slots: [{ index: 0, kind: "style", walk: [],
        exprObjectFields: [{ name: property, expr: property === "backgroundImage" ? "'none'" : "'#ff0000'" }],
      }] },
    };
    const source = templateMountedRenderer(component, [component], [], new Map(), () => true).join("\n");
    if (property === "background") {
      assert.match(source, /Property::BackgroundImage, -1/);
      assert.match(source, /Property::BackgroundClip, 0/);
    }
    else if (property === "backgroundColor") assert.doesNotMatch(source, /Property::Background(?:Image|Clip)/);
    else assert.match(source, /applyProperty\(root, "background-image", std::string\("none"\)\)/);
  }
});

test("opaque hex inline colors lower to native style writes", () => {
  const component = {
    id: "Panel",
    module: "test",
    exportName: "Panel",
    runtimeBase: "compiled",
    template: {
      html: "<div></div>",
      slots: [
        {
          index: 0,
          kind: "style",
          walk: [],
          exprObjectFields: [
            {
              name: "backgroundColor",
              expr: "game.active ? '#ffffff' : '#000000'",
            },
            { name: "color", expr: "'#123456'" },
            {
              name: "borderColor",
              expr: "game.active ? '#ff0000' : '#00ff00'",
            },
            { name: "activeBackgroundColor", expr: "'#000000'" },
          ],
        },
      ],
    },
  };
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "active",
      fieldType: "bool",
      readerName: "read_game_active",
      field: { name: "active" },
      shape: null,
      reader: null,
    },
  ];
  const lines = templateMountedRenderer(
    component,
    [component],
    storeFields,
    new Map(),
    () => true,
  );
  assert.ok(
    lines,
    "expected the template renderer to lower opaque color styles",
  );
  const source = lines.join("\n");
  assert.match(
    source,
    /root\.style\(\)\.backgroundColor\(static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 255, 255\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.color\(static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(18, 52, 86\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::BorderColor, static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 0, 0\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 255, 0\)\)\)\);/,
  );
  assert.match(
    source,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::ActiveBackgroundColor, static_cast<int>\(gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\); root\.style\(\)\.set\(gea::embedded::ui::Property::HasActiveBackground, 1\);/,
  );
  assert.doesNotMatch(
    source,
    /applyProperty\(root, gea::embedded::ui::StyleDeclaration::(?:Background|Color|BorderColor|ActiveBackgroundColor)/,
  );
  assert.doesNotMatch(
    source,
    /applyNumberProperty\(root, gea::embedded::ui::StyleDeclaration::(?:Background|Color|BorderColor|ActiveBackgroundColor)/,
  );
});

test("mounted root and row opaque hex colors lower to native style writes", () => {
  const storeFields = [
    {
      storeClass: "GameStore",
      stateType: "GameStore",
      storeGlobalName: "game",
      fieldName: "active",
      fieldType: "bool",
      readerName: "read_game_active",
      field: { name: "active" },
      shape: null,
      reader: null,
    },
  ];
  const lowered = lowerSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          {
            name: "backgroundColor",
            expr: "game.active ? '#fff' : '#000'",
            exprPath: ["game", "active"],
          },
          {
            name: "borderTopColor",
            expr: "game.active ? '#ff0000' : '#00ff00'",
            exprPath: ["game", "active"],
          },
        ],
      },
    ],
    storeFields,
  );
  assert.ok(lowered, "expected mounted root color style to lower");
  const rootSource = lowered.lines.join("\n");
  assert.match(
    rootSource,
    /root\.style\(\)\.backgroundColor\(static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 255, 255\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\)\);/,
  );
  assert.match(
    rootSource,
    /root\.style\(\)\.set\(gea::embedded::ui::Property::BorderTopColor, static_cast<int>\(\(__gea_active \? gea::framework::graphics::pixel::nativeStyleValue\(255, 0, 0\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 255, 0\)\)\)\);/,
  );
  assert.doesNotMatch(rootSource, /StyleDeclaration::Background/);
  assert.doesNotMatch(rootSource, /StyleDeclaration::BorderTopColor/);

  const field = {
    storeClass: "GameStore",
    stateType: "GameStore",
    fieldName: "rows",
    fieldType: "std::vector<GameStore_rows_item>",
    readerName: "read_game_rows",
    itemType: "GameStore_rows_item",
    itemTypeRef: "GameStore_rows_item",
    useInterfaceStruct: false,
    itemFields: [
      { name: "visible", shape: { kind: "literal", valueType: "boolean" } },
    ],
  };
  const rows = lowerRowSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "color", expr: "item.visible ? '#fff' : '#000'" },
          {
            name: "borderRightColor",
            expr: "item.visible ? '#ff0000' : '#00ff00'",
          },
        ],
      },
    ],
    "item",
    field,
  );
  const rowSource = rows.lines.join("\n");
  assert.match(
    rowSource,
    /row\.style\(\)\.color\(static_cast<int>\(\(item\.visible \? gea::framework::graphics::pixel::nativeStyleValue\(255, 255, 255\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 0, 0\)\)\)\);/,
  );
  assert.match(
    rowSource,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::BorderRightColor, static_cast<int>\(\(item\.visible \? gea::framework::graphics::pixel::nativeStyleValue\(255, 0, 0\) : gea::framework::graphics::pixel::nativeStyleValue\(0, 255, 0\)\)\)\);/,
  );
  assert.doesNotMatch(rowSource, /StyleDeclaration::Color/);
  assert.doesNotMatch(rowSource, /StyleDeclaration::BorderRightColor/);
});

test("typed row style values avoid primitive gea_cpp_key boxing", () => {
  const field = {
    storeClass: "GameStore",
    stateType: "GameStore",
    fieldName: "rows",
    fieldType: "std::vector<GameStore_rows_item>",
    readerName: "read_game_rows",
    itemType: "GameStore_rows_item",
    itemTypeRef: "GameStore_rows_item",
    useInterfaceStruct: false,
    itemFields: [
      { name: "size", shape: { kind: "literal", valueType: "number" } },
      { name: "visible", shape: { kind: "literal", valueType: "boolean" } },
    ],
  };
  const lowered = lowerRowSlots(
    [
      {
        index: 0,
        kind: "style",
        walk: [],
        exprObjectFields: [
          { name: "width", expr: "item.size" },
          { name: "height", expr: "`${item.size}px`" },
          { name: "top", expr: "`${item.size}%`" },
          { name: "translateY", expr: "`${item.size}px`" },
          { name: "transform", expr: "`rotate(${item.size}deg)`" },
          { name: "rotate", expr: "`${item.size}deg`" },
          { name: "padding", expr: "item.size" },
          { name: "marginLeft", expr: "item.size" },
          { name: "borderRadius", expr: "item.size" },
          { name: "borderWidth", expr: "item.size" },
          { name: "borderLeftWidth", expr: "`${item.size}px`" },
          { name: "zIndex", expr: "item.size" },
          { name: "flex", expr: "item.size" },
          { name: "opacity", expr: "item.visible" },
          { name: "color", expr: "`${item.size}px`" },
        ],
      },
    ],
    "item",
    field,
  );

  const source = lowered.lines.join("\n");
  assert.match(source, /applyNumberProperty\(row, gea::embedded::ui::StyleDeclaration::BorderWidth, static_cast<double>\(item.size\)\)/);
  assert.match(source, /applyPixelLengthProperty\(row, gea::embedded::ui::StyleDeclaration::BorderLeftWidth, static_cast<double>\(item.size\)\)/);
  assert.match(
    source,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::Width, .*static_cast<double>\(item\.size\)/,
  );
  assert.match(
    source,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::Height, .*static_cast<double>\(item\.size\)/,
  );
  assert.match(
    source,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::TopPercent, static_cast<int>\(std::round\(static_cast<double>\(item\.size\) \* 10\.0\)\)\);/,
  );
  assert.match(
    source,
    /row\.style\(\)\.translateY\(.*static_cast<double>\(item\.size\)/,
  );
  assert.match(source, /row\.style\(\)\.cssRotateDegrees\(/);
  assert.equal(
    [
      ...source.matchAll(
        /row\.style\(\)\.(?:rotateDegrees|cssRotateDegrees)\(static_cast<double>\(item\.size\)\);/g,
      ),
    ].length,
    2,
  );
  for (const property of [
    "PaddingTop",
    "PaddingRight",
    "PaddingBottom",
    "PaddingLeft",
    "MarginLeft",
    "BorderRadiusTopLeft",
    "BorderRadiusTopRight",
    "BorderRadiusBottomRight",
    "BorderRadiusBottomLeft",
    "ZIndex",
    "Flex",
  ]) {
    assert.match(
      source,
      new RegExp(
        `row\\.style\\(\\)\\.set\\(gea::embedded::ui::Property::${property}, .*static_cast<double>\\(item\\.size\\)`,
      ),
    );
  }
  assert.match(
    source,
    /row\.style\(\)\.set\(gea::embedded::ui::Property::FlexBasis, gea::embedded::ui::kUnset\);/,
  );
  assert.match(
    source,
    /applyProperty\(row, gea::embedded::ui::StyleDeclaration::Opacity, gea_cpp_to_string\(item\.visible\)\);/,
  );
  assert.match(source, /gea_cpp_to_string\(item\.size\)/);
  assert.equal([...source.matchAll(/applyNumberProperty\(row/g)].length, 1);
  assert.doesNotMatch(source, /gea_cpp_key\(item\.(?:size|visible)\)/);
});

test("local keyed-list component treats JSX key as reconciler metadata", () => {
  const component = {
    id: "board.tsx#TetrisStack",
    module: "board.tsx",
    exportName: "TetrisStack",
    runtimeBase: "static",
    template: {
      html: "<div class=tetris-stack-layer>",
      slots: [
        {
          index: 0,
          kind: "keyed-list",
          walk: [],
          expr: "tetris.cells",
          exprPath: ["tetris", "cells"],
          payload: {
            itemParam: "cell",
            rowTemplate: {
              html: "<div>",
              slots: [
                {
                  index: 0,
                  kind: "attr",
                  walk: [],
                  expr: "cell.id",
                  exprPath: ["cell", "id"],
                  payload: { attrName: "key" },
                },
                {
                  index: 1,
                  kind: "class",
                  walk: [],
                  expr: "{ 'tetris-block': true, 'tetris-block-hidden': cell.filled === 0 }",
                  exprObjectFields: [
                    { name: "tetris-block", expr: "true" },
                    { name: "tetris-block-hidden", expr: "cell.filled === 0" },
                  ],
                },
                {
                  index: 2,
                  kind: "style",
                  walk: [],
                  expr: "{ left: cell.left, top: cell.top, backgroundColor: cell.color }",
                  exprObjectFields: [
                    { name: "left", expr: "cell.left" },
                    { name: "top", expr: "cell.top" },
                    { name: "backgroundColor", expr: "cell.color" },
                  ],
                },
              ],
            },
          },
        },
      ],
    },
  };
  const stores = [
    {
      id: "TetrisStore",
      module: "store.ts",
      className: "TetrisStore",
      runtimeBase: "compiled",
      instanceName: "tetris",
      fields: [
        {
          name: "cells",
          shape: {
            kind: "array",
            element: {
              kind: "object",
              fields: [
                { name: "id", shape: { kind: "literal", valueType: "number" } },
                {
                  name: "left",
                  shape: { kind: "literal", valueType: "number" },
                },
                {
                  name: "top",
                  shape: { kind: "literal", valueType: "number" },
                },
                {
                  name: "color",
                  shape: { kind: "literal", valueType: "string" },
                },
                {
                  name: "filled",
                  shape: { kind: "literal", valueType: "number" },
                },
              ],
            },
          },
        },
      ],
    },
  ];

  const renderer = mountedRendererForComponent(
    component,
    [component],
    collectStoreFields(stores),
    collectStoreArraySources(stores),
    new Map(),
    [],
  );
  assert.ok(renderer, "expected a native renderer for a keyed local component");
  const source = renderer.join("\n");
  assert.match(source, /mount_TetrisStack/);
  assert.match(source, /cell\.id/);
  assert.doesNotMatch(source, /setAttribute\("key"/);
});

test("authored numeric zero remains visible in a typed keyed row", () => {
  const component = {
    id: "CounterList",
    module: "counter-list.tsx",
    exportName: "CounterList",
    runtimeBase: "compiled",
    template: {
      html: "<div><!--0--></div>",
      slots: [
        {
          index: 0,
          kind: "keyed-list",
          walk: [0],
          expr: "reader.counts",
          exprPath: ["reader", "counts"],
          payload: {
            itemParam: "count",
            rowTemplate: { html: "<span>0</span>", slots: [] },
          },
        },
      ],
    },
  };

  const counts = {
    storeClass: "ReaderStore",
    stateType: "ReaderStore_state",
    storeGlobalName: "reader",
    fieldName: "counts",
    fieldType: "std::vector<double>",
    readerName: "read_ReaderStore_field_counts",
    itemType: "double",
    itemTypeRef: "double",
    useInterfaceStruct: false,
    itemFields: [],
    storeRuntimeBase: "compiled",
    storeIsSelfStore: false,
  };
  const renderer = templateMountedRenderer(
    component,
    [component],
    [],
    new Map(),
    () => true,
    [],
    undefined,
    [counts],
  );
  assert.ok(renderer, "expected a native renderer for the typed keyed list");
  assert.match(renderer.join("\n"), /setText\("0"\)/);
});

test("keyed child component keeps its typed item prop with additional static props", () => {
  const parent = {
    id: "LibraryScreen",
    module: "test",
    exportName: "LibraryScreen",
    runtimeBase: "compiled",
    template: {
      html: "<div><span>0</span><!--1--></div>",
      slots: [
        {
          index: 0,
          kind: "text",
          walk: [0, 0],
          expr: "reader.heading",
          exprPath: ["reader", "heading"],
          directText: true,
        },
        {
          index: 1,
          kind: "keyed-list",
          walk: [1],
          expr: "reader.visibleBooks",
          exprPath: ["reader", "visibleBooks"],
          payload: {
            itemParam: "book",
            keyExpr: "book.id",
            rowTemplate: {
              html: "<!--0-->",
              slots: [
                {
                  index: 0,
                  kind: "mount",
                  walk: [],
                  payload: {
                    tag: "BookCover",
                    attrs: [
                      { code: "key={book.id}" },
                      { code: "book={book}" },
                      { code: "compact={1}" },
                    ],
                  },
                },
              ],
            },
          },
        },
      ],
    },
  };
  const child = {
    id: "BookCover",
    module: "test",
    exportName: "BookCover",
    runtimeBase: "compiled",
    template: {
      html: "<article><span>0</span><span><!--2-->% READ</span></article>",
      slots: [
        {
          index: 0,
          kind: "class",
          walk: [],
          expr: "this.props.compact ? 'cover compact' : 'cover'",
        },
        {
          index: 1,
          kind: "text",
          walk: [0, 0],
          expr: "this.props.book.title",
          directText: true,
        },
        {
          index: 2,
          kind: "text",
          walk: [1, 0],
          expr: "this.props.book.progressPercent",
          directText: false,
        },
      ],
    },
  };
  const heading = {
    storeClass: "ReaderStore",
    stateType: "ReaderStore_state",
    storeGlobalName: "reader",
    fieldName: "heading",
    fieldType: "std::string",
    readerName: "read_ReaderStore_field_heading",
    field: { name: "heading" },
    shape: { kind: "literal", valueType: "string" },
    reader: null,
    storeRuntimeBase: "compiled",
    storeIsSelfStore: false,
  };
  const visibleBooks = {
    storeClass: "ReaderStore",
    stateType: "ReaderStore_state",
    storeGlobalName: "reader",
    fieldName: "visibleBooks",
    fieldType: "std::vector<::__gea_type_BookRow>",
    readerName: "read_ReaderStore_field_visibleBooks",
    itemType: "__gea_type_BookRow",
    itemTypeRef: "::__gea_type_BookRow",
    useInterfaceStruct: true,
    itemFields: [
      { name: "id", shape: { kind: "literal", valueType: "string" } },
      { name: "title", shape: { kind: "literal", valueType: "string" } },
      {
        name: "progressPercent",
        shape: { kind: "literal", valueType: "number" },
      },
    ],
    storeRuntimeBase: "compiled",
    storeIsSelfStore: false,
  };

  const lines = templateMountedRenderer(
    parent,
    [parent, child],
    [heading],
    new Map(),
    () => true,
    [],
    undefined,
    [visibleBooks],
  );
  assert.ok(
    lines,
    "expected the mixed parent template to lower its keyed child row",
  );
  const source = lines.join("\n");
  assert.match(source, /const auto &book = __gea_items\[__gea_index\]/);
  assert.match(source, /book\.title/);
  assert.match(
    source,
    /auto (__gea_kl_row_\d+) = gea::embedded::ui::Document::instance\(\)\.createText\(\);[\s\S]*\1\.setText\(\(gea_cpp_to_string\(book\.progressPercent\)\)\.c_str\(\)\);/,
  );
  assert.match(source, /static_cast<double>\(1\)/);
  assert.doesNotMatch(source, /gea_cpp_key\(book\)/);
  assert.doesNotMatch(source, /record_set_literal\("book"/);
  assert.doesNotMatch(source, /std::make_shared<BookCover>/);
  assert.doesNotMatch(source, /BookCover.*(?:render|dispose)/);

  const inlineOnly = inlineOnlyComponentClassesForIr({
    schema: "gea-ir",
    version: 1,
    entry: "main.ts",
    modules: [],
    components: [parent, child],
    stores: [
      {
        id: "ReaderStore",
        module: "reader-store.ts",
        className: "ReaderStore",
        runtimeBase: "compiled",
        fields: [
          { name: "heading", shape: { kind: "literal", valueType: "string" } },
          {
            name: "visibleBooks",
            shape: {
              kind: "array",
              elementTypeName: "BookRow",
              element: {
                kind: "object",
                fields: visibleBooks.itemFields,
              },
            },
          },
        ],
      },
    ],
    hostCapabilities: [],
  });
  assert.ok(
    inlineOnly.has("BookCover"),
    "prop-driven row child should not retain an unreachable boxed class module",
  );
});

test("typed keyed child props fail explicitly instead of falling back to boxed lifecycle code", () => {
  const parent = {
    id: "LibraryScreen",
    module: "library-screen.tsx",
    exportName: "LibraryScreen",
    runtimeBase: "compiled",
    template: {
      html: "<div><span>0</span><!--1--></div>",
      slots: [
        {
          index: 0,
          kind: "text",
          walk: [0, 0],
          expr: "formatHeading(reader.heading)",
        },
        {
          index: 1,
          kind: "keyed-list",
          walk: [1],
          expr: "reader.visibleBooks",
          exprPath: ["reader", "visibleBooks"],
          payload: {
            itemParam: "book",
            rowTemplate: {
              html: "<!--0-->",
              slots: [
                {
                  index: 0,
                  kind: "mount",
                  walk: [],
                  payload: {
                    tag: "BookCover",
                    attrs: [{ code: "book={book}" }, { code: "compact={1}" }],
                  },
                },
              ],
            },
          },
        },
      ],
    },
  };
  const child = {
    id: "BookCover",
    module: "book-cover.tsx",
    exportName: "BookCover",
    runtimeBase: "compiled",
    template: {
      html: "<article>0</article>",
      slots: [
        { index: 0, kind: "text", walk: [0], expr: "this.props.book.title" },
      ],
    },
  };
  const ir = {
    schema: "gea-ir",
    version: 1,
    entry: "main.ts",
    modules: [],
    components: [parent, child],
    stores: [
      {
        id: "ReaderStore",
        module: "reader-store.ts",
        className: "ReaderStore",
        runtimeBase: "compiled",
        instanceName: "reader",
        fields: [
          { name: "heading", shape: { kind: "literal", valueType: "string" } },
          {
            name: "visibleBooks",
            shape: {
              kind: "array",
              element: {
                kind: "object",
                fields: [
                  {
                    name: "id",
                    shape: { kind: "literal", valueType: "string" },
                  },
                  {
                    name: "title",
                    shape: { kind: "literal", valueType: "string" },
                  },
                ],
              },
            },
          },
        ],
      },
    ],
    hostCapabilities: [],
  };

  const diagnostics = diagnoseBoxedKeyedComponentProps(ir);
  assert.equal(diagnostics.length, 1);
  assert.equal(diagnostics[0].code, "gea-boxed-keyed-component-props");
  assert.equal(diagnostics[0].severity, "error");
  assert.match(diagnostics[0].message, /refuses to box/);
  assert.match(diagnostics[0].message, /<BookCover book=\{book\}/);
});

test("mixed generic child fallback crosses TUs through the gea_ir DOM bridge", () => {
  const parent = {
    id: "MixedParent",
    module: "mixed-parent.tsx",
    exportName: "MixedParent",
    runtimeBase: "compiled",
    template: {
      html: "<div><!--0--></div>",
      slots: [
        {
          index: 0,
          kind: "mount",
          walk: [0],
          payload: { tag: "GenericChild", attrs: [] },
        },
      ],
    },
  };
  const child = {
    id: "GenericChild",
    module: "generic-child.tsx",
    exportName: "GenericChild",
    runtimeBase: "compiled",
    template: {
      html: "<span>child</span>",
      slots: [],
    },
  };

  const lines = templateMountedRenderer(
    parent,
    [parent, child],
    [],
    new Map(),
    (component) => component !== child,
  );
  assert.ok(lines, "expected the generic child fallback to lower");
  const source = lines.join("\n");
  assert.match(source, /std::make_shared<GenericChild>/);
  assert.match(source, /->render\(gea_ir::hostDocumentNodeValue\(/);
  assert.doesNotMatch(source, /gea::runtime::host::domNodeValue/);
});
