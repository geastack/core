import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";

export function fileAssetDirectory(manifest) {
  const storage = manifest?.gea?.embeddedAssets?.storage;
  if (storage === undefined) return null;
  if (
    storage?.type !== "file" ||
    typeof storage.directory !== "string" ||
    !storage.directory.startsWith("/") ||
    storage.directory === "/" ||
    storage.directory !== path.posix.normalize(storage.directory) ||
    /[\\\0\r\n]/.test(storage.directory)
  ) {
    throw new Error(
      'gea.embeddedAssets.storage must specify type="file" and a normalized absolute directory',
    );
  }
  return storage.directory;
}

export function fileAssetBundle(appDir, outCpp, kind, entries) {
  const manifestFile = path.join(appDir, "package.json");
  const manifest = fs.existsSync(manifestFile)
    ? JSON.parse(fs.readFileSync(manifestFile, "utf8"))
    : {};
  const directory = fileAssetDirectory(manifest);
  const metadata = `${outCpp}.assets.json`;
  if (!directory || !entries.length) {
    // A manifest left from an earlier storage-backed build would still have
    // deploy tooling push that bundle.
    fs.rmSync(metadata, { force: true });
    return null;
  }
  const bytes = Buffer.concat(entries.map((entry) => Buffer.from(entry.data)));
  const hash = createHash("sha256").update(bytes).digest("hex");
  const id = String(manifest.gea?.id || path.basename(appDir)).replace(
    /[^a-zA-Z0-9_-]/g,
    "_",
  );
  const filename = `${id}-${kind}-${hash}.bin`;
  const local = path.join(path.dirname(outCpp), filename);
  const remote = path.posix.join(directory, filename);
  fs.mkdirSync(path.dirname(local), { recursive: true });
  if (!fs.existsSync(local) || !fs.readFileSync(local).equals(bytes))
    fs.writeFileSync(local, bytes);
  const deployment = {
    version: 1,
    files: [{ local, remote, bytes: bytes.length, sha256: hash }],
  };
  const contents = JSON.stringify(deployment, null, 2) + "\n";
  if (
    !fs.existsSync(metadata) ||
    fs.readFileSync(metadata, "utf8") !== contents
  )
    fs.writeFileSync(metadata, contents);
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit++)
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return {
    entries,
    remote,
    bytes: bytes.length,
    crc: (crc ^ 0xffffffff) >>> 0,
  };
}

export function fileAssetLoader(bundle, name) {
  return [
    `static bool ${name}() {`,
    "  static const bool ready = [] {",
    "    const gea::platform::storage::AssetSegment segments[] = {",
    ...bundle.entries.map(
      (entry) => `      {${entry.symbol}, ${entry.data.length}},`,
    ),
    "    };",
    `    return gea::platform::storage::loadAssetBundle(${JSON.stringify(bundle.remote)}, segments, ${bundle.entries.length}, ${bundle.bytes}, ${bundle.crc}U);`,
    "  }();",
    "  return ready;",
    "}",
    "",
  ];
}
