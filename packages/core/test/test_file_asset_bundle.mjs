import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  fileAssetBundle,
  fileAssetDirectory,
} from "../scripts/gea-file-asset-bundle.mjs";

const core = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const fixture = fs.mkdtempSync(path.join(os.tmpdir(), "gea-file-assets-"));
// The storage directory is the device's filesystem path, which the manifest
// requires to be POSIX (/nand/assets); this host test stands its fixture in
// for that filesystem. On Windows the fixture's root-relative spelling
// (/Users/.../Temp/...) is that POSIX path, and with the fixture's drive
// current, node and the test programs open it there. Executables carry .exe,
// the only name spawn finds.
const windows = process.platform === "win32";
const fixtureRoot = path.parse(fixture).root;
const directory = windows
  ? fixture.slice(fixtureRoot.length - 1).replaceAll("\\", "/")
  : fixture;
if (windows) process.chdir(fixtureRoot);
const exe = windows ? ".exe" : "";
try {
  const storage = { type: "file", directory };
  assert.equal(fileAssetDirectory({}), null);
  for (const directory of [
    "relative",
    "/",
    "/nand/../etc",
    "/nand\\bad",
    "/nand\n",
  ]) {
    assert.throws(() =>
      fileAssetDirectory({
        gea: { embeddedAssets: { storage: { ...storage, directory } } },
      }),
    );
  }
  fs.writeFileSync(
    path.join(fixture, "package.json"),
    JSON.stringify({ gea: { id: "test", embeddedAssets: { storage } } }),
  );
  const outCpp = path.join(fixture, "assets.cpp");
  const entries = [
    { symbol: "asset_one", data: Buffer.from("123") },
    { symbol: "asset_two", data: Buffer.from("456789") },
  ];
  const bundle = fileAssetBundle(fixture, outCpp, "images", entries);
  assert.equal(bundle.crc, 0xcbf43926);
  assert.equal(bundle.bytes, 9);
  assert.equal(fs.readFileSync(bundle.remote, "utf8"), "123456789");
  const again = fileAssetBundle(fixture, outCpp, "images", entries);
  assert.equal(again.remote, bundle.remote);
  const changed = fileAssetBundle(fixture, outCpp, "images", [
    { symbol: "asset_one", data: Buffer.from("next") },
  ]);
  assert.notEqual(changed.remote, bundle.remote);
  assert.equal(
    fs.readFileSync(bundle.remote, "utf8"),
    "123456789",
    "previous firmware keeps its own bundle for rollback",
  );
  const binary = path.join(fixture, "test" + exe);
  execFileSync(
    process.env.CXX || "clang++",
    [
      "-std=c++20",
      "-O2",
      "-fsanitize=address,undefined",
      "-I" + path.join(core, "include"),
      path.join(core, "test/test_file_asset_bundle_main.cpp"),
      "-o",
      binary,
    ],
    { stdio: "inherit" },
  );
  execFileSync(binary, [path.join(fixture, "payload.bin")], {
    stdio: "inherit",
  });

  // Exercise the real generators, emitted C++ symbols, and lazy font lookups.
  const app = path.join(fixture, "app");
  fs.mkdirSync(path.join(app, "public"), { recursive: true });
  fs.copyFileSync(
    path.join(core, "test/website/fonts/bebas-neue-400.ttf"),
    path.join(app, "font.ttf"),
  );
  fs.writeFileSync(
    path.join(app, "package.json"),
    JSON.stringify({
      gea: {
        id: "generated-test",
        embeddedAssets: { storage, compileTimeOnly: ["font.ttf"] },
      },
    }),
  );
  fs.writeFileSync(
    path.join(app, "styles.css"),
    "@font-face { font-family: 'Bebas Neue'; src: url('./font.ttf'); }\nbody { font-family: 'Bebas Neue'; font-size: 20px; }\n",
  );
  fs.writeFileSync(
    path.join(app, "public/pixel.png"),
    Buffer.from(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aO1sAAAAASUVORK5CYII=",
      "base64",
    ),
  );
  const imagesCpp = path.join(fixture, "images.cpp");
  const fontsCpp = path.join(fixture, "fonts.cpp");
  const fontsHeader = path.join(fixture, "gea_embedded_font_generated.h");
  const generate = (script, args) =>
    execFileSync(
      process.execPath,
      [path.join(core, "scripts", script), "--app-dir", app, ...args],
      { stdio: "pipe" },
    );
  generate("generate-gea-embedded-assets.mjs", ["--out-cpp", imagesCpp]);
  generate("generate-gea-embedded-fonts.mjs", [
    "--css-dir",
    app,
    "--out-cpp",
    fontsCpp,
    "--out-h",
    fontsHeader,
  ]);
  const files = [imagesCpp, fontsCpp].map(
    (file) =>
      JSON.parse(fs.readFileSync(file + ".assets.json", "utf8")).files[0],
  );
  const payloads = files.map((file) => fs.readFileSync(file.local));
  const generatedBinary = path.join(fixture, "generated-test" + exe);
  execFileSync(
    process.env.CXX || "clang++",
    [
      "-std=c++20",
      "-O2",
      "-fsanitize=address,undefined",
      "-I" + path.join(core, "include"),
      "-I" + fixture,
      imagesCpp,
      fontsCpp,
      path.join(core, "test/test_file_asset_generated_main.cpp"),
      "-o",
      generatedBinary,
    ],
    { stdio: "inherit" },
  );
  const run = (state) =>
    execFileSync(generatedBinary, [state, files[0].remote, files[1].remote], {
      stdio: "inherit",
    });
  for (const file of files) fs.rmSync(file.local);
  run("missing");
  files.forEach((file, i) =>
    fs.writeFileSync(file.local, Buffer.from(payloads[i]).fill(0, 0, 8)),
  );
  run("corrupt");
  files.forEach((file, i) => fs.writeFileSync(file.local, payloads[i]));
  run("ready");

  // gea_app_entry.cpp calls the prepare hook whether or not the app uses file
  // storage, and Apple's ld rejects a weak reference nothing defines, so the
  // generator has to define it for apps without a bundle too.
  const prepareMain = path.join(fixture, "prepare_main.cpp");
  fs.writeFileSync(
    prepareMain,
    'extern "C" bool gea_embedded_assets_prepare();\nint main() { return gea_embedded_assets_prepare() ? 0 : 1; }\n',
  );
  for (const [id, asset] of [
    ["no-storage-test", "public/pixel.png"],
    ["no-assets-test", null],
  ]) {
    const plain = path.join(fixture, id);
    fs.mkdirSync(path.join(plain, "public"), { recursive: true });
    fs.writeFileSync(
      path.join(plain, "package.json"),
      JSON.stringify({ gea: { id } }),
    );
    if (asset) fs.copyFileSync(path.join(app, asset), path.join(plain, asset));
    const plainCpp = path.join(fixture, id + ".cpp");
    execFileSync(
      process.execPath,
      [
        path.join(core, "scripts/generate-gea-embedded-assets.mjs"),
        "--app-dir",
        plain,
        "--out-cpp",
        plainCpp,
      ],
      { stdio: "pipe" },
    );
    const plainBinary = path.join(fixture, id + "-prepare" + exe);
    execFileSync(
      process.env.CXX || "clang++",
      ["-std=c++20", plainCpp, prepareMain, "-o", plainBinary],
      { stdio: "inherit" },
    );
    execFileSync(plainBinary, [], { stdio: "inherit" });
  }
  console.log(
    "File asset bundles: validated content, rollback identity, corrupt/truncated/oversized/missing files, ASan/UBSan passed",
  );
} finally {
  fs.rmSync(fixture, { recursive: true, force: true });
}
