import fs from "node:fs";

function integer(value, name, minimum = 0, maximum = 65535) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`Invalid bitmap font ${name}`);
  }

  return value;
}

export function loadBitmapFontSource(filename) {
  const source = JSON.parse(fs.readFileSync(filename, "utf8"));

  if (
    source.schemaVersion !== 1 ||
    source.format !== "gea-bitmap-font" ||
    !Array.isArray(source.fonts)
  ) {
    throw new Error(`Invalid bitmap font source: ${filename}`);
  }

  return source;
}

export function buildBitmapFont(source, tuple, packGlyphAtlas) {
  const face = source.fonts.find((font) => font.sizePx === tuple.sizePx);

  if (!face)
    throw new Error(
      `Bitmap font ${tuple.family} has no exact ${tuple.sizePx}px face`,
    );
  const lineHeight = integer(face.lineHeight, "lineHeight", 1, 1024);
  const ascender = integer(face.ascender, "ascender", 0, lineHeight);
  const descender = integer(face.descender, "descender", 0, lineHeight);
  if (ascender + descender !== lineHeight)
    throw new Error("Invalid bitmap font line metrics");
  const glyphs = [];
  const bitmaps = [];
  const available = new Set();
  const requested = tuple.charset;
  const fallbackCodepoint =
    face.fallbackCodepoint === undefined
      ? -1
      : integer(face.fallbackCodepoint, "fallbackCodepoint", 0, 0x10ffff);

  if (!Array.isArray(face.glyphs) || face.glyphs.length === 0)
    throw new Error("Bitmap font has no glyphs");
  for (const item of face.glyphs) {
    const cp = integer(item.codepoint, "codepoint", 0, 0x10ffff);

    if (available.has(cp)) throw new Error("Duplicate bitmap font glyph");
    available.add(cp);
    const width = integer(item.width, "glyph width", 0, 512);
    const height = integer(item.height, "glyph height", 0, 512);
    const advance16 = integer(item.advance16, "advance16", 0, 32767);
    const bearingX = integer(item.bearingX, "bearingX", -1024, 1024);
    const bearingY = integer(item.bearingY, "bearingY", -1024, 1024);
    if (
      typeof item.coverage !== "string" ||
      !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(
        item.coverage,
      )
    ) {
      throw new Error("Invalid bitmap glyph coverage encoding");
    }

    const coverage = Buffer.from(item.coverage, "base64");

    if (coverage.length !== width * height)
      throw new Error("Invalid bitmap glyph coverage length");
    if (
      requested &&
      !requested.has(cp) &&
      cp !== 63 &&
      cp !== fallbackCodepoint
    )
      continue;
    glyphs.push({
      codepoint: cp,
      sourceX: 0,
      sourceY: 0,
      width,
      height,
      advance: (advance16 + 8) >> 4,
      bearingX,
      bearingY,
      advance16,
    });
    bitmaps.push(coverage);
  }

  if (fallbackCodepoint >= 0 && !available.has(fallbackCodepoint))
    throw new Error("Bitmap fallback glyph is absent");
  if (glyphs.length === 0)
    throw new Error("Bitmap font charset selects no available glyphs");
  const records = glyphs
    .map((glyph, index) => ({ glyph, bitmap: bitmaps[index] }))
    .sort((a, b) => a.glyph.codepoint - b.glyph.codepoint);
  const sortedGlyphs = records.map((record) => record.glyph);
  const { atlasWidth, atlasHeight } = packGlyphAtlas(sortedGlyphs);
  const width = Math.max(1, atlasWidth);
  const height = Math.max(1, atlasHeight);
  const atlasData = new Uint8Array(width * height);

  for (const { glyph, bitmap } of records) {
    for (let row = 0; row < glyph.height; row++) {
      atlasData.set(
        bitmap.subarray(row * glyph.width, (row + 1) * glyph.width),
        (glyph.sourceY + row) * width + glyph.sourceX,
      );
    }
  }

  const selected = new Set(sortedGlyphs.map((glyph) => glyph.codepoint));
  const kerning = [];
  const pairs = new Set();
  for (const pair of face.kerning || []) {
    const left = integer(pair.left, "kerning left", 0, 0x10ffff);
    const right = integer(pair.right, "kerning right", 0, 0x10ffff);
    const adjustment16 = integer(
      pair.adjustment16,
      "kerning adjustment",
      -32768,
      32767,
    );
    const key = `${left}:${right}`;

    if (pairs.has(key)) throw new Error("Duplicate bitmap font kerning pair");
    pairs.add(key);
    if (!available.has(left) || !available.has(right))
      throw new Error("Kerning references absent glyph");
    if (selected.has(left) && selected.has(right) && adjustment16 !== 0)
      kerning.push({ left, right, adjustment16 });
  }

  kerning.sort((a, b) => a.left - b.left || a.right - b.right);

  return {
    id: tuple.id,
    family: tuple.family,
    familyId: tuple.familyId,
    sizePx: tuple.sizePx,
    lineHeight,
    ascender,
    descender,
    glyphs: sortedGlyphs,
    atlasWidth: width,
    atlasHeight: height,
    atlasBits: 8,
    atlasData,
    kerning,
    importedBitmap: true,
    fallbackCodepoint,
  };
}
