import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import vm from "node:vm";
import test from "node:test";

import {
  buildBitmapFont,
  loadBitmapFontSource,
} from "../scripts/bitmap-font-source.mjs";

const root = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../../..",
);
const app = path.resolve(root, "../examples/apps/m5-stopwatch");
const script = path.join(
  root,
  "packages/core/scripts/generate-gea-embedded-fonts.mjs",
);

function generator(source) {
  source = source
    .replace(/^import[\s\S]*?from\s+['"][^'"]+['"];?\s*\n/gm, "")
    .replaceAll(
      "import.meta.url",
      JSON.stringify(new URL(`file://${script}`).href),
    )
    .replace(/writeGeneratedFonts\(\);?\s*$/, "");
  const context = vm.createContext({
    fs,
    path,
    process: {
      ...process,
      argv: [
        "node",
        script,
        "--app-dir",
        app,
        "--css-dir",
        app,
        "--out-cpp",
        "/dev/stdout",
        "--out-h",
        "/dev/null",
      ],
    },
    Buffer,
    Uint8Array,
    createHash,
    createRequire,
    fileURLToPath,
    execFileSync,
    buildBitmapFont,
    loadBitmapFontSource,
    console,
  });

  vm.runInContext(
    `${source}\nglobalThis.api = { rasterizeFont, generatedCpp, packGlyphAtlas, collectSourceFontFaces, collectUsedFontTuples };`,
    context,
  );

  return context.api;
}

const current = generator(fs.readFileSync(script, "utf8"));

test("default TTF rasterization and emitted C++ remain byte-for-byte unchanged", () => {
  const previous = generator(
    execFileSync(
      "git",
      ["show", "HEAD:packages/core/scripts/generate-gea-embedded-fonts.mjs"],
      { cwd: root, encoding: "utf8" },
    ),
  );
  const opentype = createRequire(path.join(app, "package.json"))("opentype.js");
  const fontPath = path.join(app, "assets/Montserrat-Medium.ttf");
  const charset = new Set(
    Array.from("AV 2000/1/28 FRI", (character) => character.codePointAt(0)),
  );
  const oldFont = previous.rasterizeFont(
    opentype,
    fontPath,
    "Montserrat",
    0,
    24,
    0,
    charset,
  );
  const newFont = current.rasterizeFont(
    opentype,
    fontPath,
    "Montserrat",
    0,
    24,
    0,
    charset,
  );

  assert.equal(
    current.generatedCpp([newFont], [{ id: 0, name: "Montserrat" }]),
    previous.generatedCpp([oldFont], [{ id: 0, name: "Montserrat" }]),
  );
});

test("source bitmap atlas preserves every source glyph byte, metrics and pair adjustment", () => {
  for (const name of [
    "factory-montserrat.json",
    "factory-maple.json",
    "factory-commissioner.json",
    "factory-montserrat-semibold.json",
  ]) {
    const source = loadBitmapFontSource(path.join(app, "assets", name));

    for (const face of source.fonts) {
      const built = buildBitmapFont(
        source,
        { id: 0, family: source.family, familyId: 0, sizePx: face.sizePx },
        current.packGlyphAtlas,
      );

      for (const item of face.glyphs) {
        const glyph = built.glyphs.find(
          (value) => value.codepoint === item.codepoint,
        );
        const coverage = [];

        for (let row = 0; row < glyph.height; row++) {
          for (let col = 0; col < glyph.width; col++) {
            coverage.push(
              built.atlasData[
                (glyph.sourceY + row) * built.atlasWidth + glyph.sourceX + col
              ],
            );
          }
        }

        assert.deepEqual(
          Buffer.from(coverage),
          Buffer.from(item.coverage, "base64"),
        );
        assert.equal(glyph.advance16, item.advance16);
        assert.equal(glyph.bearingY, item.bearingY);
      }

      assert.equal(built.lineHeight, face.lineHeight);
      assert.equal(built.ascender, face.ascender);
      assert.equal(built.fallbackCodepoint, face.fallbackCodepoint);
      assert.equal(built.kerning.length, face.kerning.length);
    }
  }
});

test("bitmap source fails closed for unavailable sizes, malformed coverage and duplicate pairs", () => {
  const source = loadBitmapFontSource(
    path.join(app, "assets/factory-commissioner.json"),
  );
  const tuple = { id: 0, family: "Commissioner", familyId: 0, sizePx: 64 };

  assert.throws(
    () =>
      buildBitmapFont(source, { ...tuple, sizePx: 63 }, current.packGlyphAtlas),
    /exact/,
  );
  const corrupt = structuredClone(source);

  corrupt.fonts[0].glyphs[0].coverage = "AAAA";
  assert.throws(
    () => buildBitmapFont(corrupt, tuple, current.packGlyphAtlas),
    /length/,
  );
  const duplicate = structuredClone(source);

  duplicate.fonts[0].kerning = [
    { left: 48, right: 49, adjustment16: -4 },
    { left: 48, right: 49, adjustment16: -4 },
  ];
  assert.throws(
    () => buildBitmapFont(duplicate, tuple, current.packGlyphAtlas),
    /Duplicate/,
  );
});

test("actual application font tuples resolve exact source sizes with generated fixed-point metadata", () => {
  const faces = current.collectSourceFontFaces();
  const tuples = current.collectUsedFontTuples(faces);
  const fonts = tuples.map((tuple, id) =>
    buildBitmapFont(
      loadBitmapFontSource(faces.embeddedSources.get(tuple.family)),
      { ...tuple, id, familyId: 0 },
      current.packGlyphAtlas,
    ),
  );
  const cpp = current.generatedCpp(fonts, [{ id: 0, name: "Imported" }]);

  assert.equal(fonts.length, 16);
  assert.ok(fonts.every((font) => font.importedBitmap));
  assert.match(cpp, /FontKerningPair font_kerning_/);
  assert.match(cpp, /true, 65533/);
});
