import assert from "node:assert/strict";
import test from "node:test";

import { compileTimeOnlyAssetPaths } from "../scripts/gea-embedded-asset-policy.mjs";

test("ordinary apps preserve all runtime assets unless exact exclusions are declared", () => {
  assert.deepEqual([...compileTimeOnlyAssetPaths({})], []);
  const manifest = {
    gea: {
      embeddedAssets: {
        compileTimeOnly: ["assets/font.ttf", "assets/font.ttf"],
      },
    },
  };

  assert.deepEqual(
    [...compileTimeOnlyAssetPaths(manifest)],
    ["assets/font.ttf"],
  );
});

test("asset policy rejects traversal, absolute paths and malformed declarations", () => {
  for (const entry of [
    "",
    "../font.ttf",
    "/font.ttf",
    "assets/../font.ttf",
    "assets\\font.ttf",
    ".",
    "..",
    7,
  ]) {
    assert.throws(() =>
      compileTimeOnlyAssetPaths({
        gea: { embeddedAssets: { compileTimeOnly: [entry] } },
      }),
    );
  }

  assert.throws(() =>
    compileTimeOnlyAssetPaths({
      gea: { embeddedAssets: { compileTimeOnly: "assets/font.ttf" } },
    }),
  );
});
