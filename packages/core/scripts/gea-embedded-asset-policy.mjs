import path from "node:path";

export function compileTimeOnlyAssetPaths(manifest) {
  const configured = manifest?.gea?.embeddedAssets?.compileTimeOnly;

  if (configured === undefined) {
    return new Set();
  }

  if (!Array.isArray(configured)) {
    throw new Error(
      "gea.embeddedAssets.compileTimeOnly must be an array of app-relative file paths",
    );
  }

  const result = new Set();

  for (const entry of configured) {
    if (
      typeof entry !== "string" ||
      entry.length === 0 ||
      entry.includes("\\") ||
      path.posix.isAbsolute(entry) ||
      path.posix.normalize(entry) !== entry ||
      entry === "." ||
      entry === ".." ||
      entry.startsWith("../")
    ) {
      throw new Error(
        "Compile-time asset paths must be normalized relative paths inside the app",
      );
    }

    result.add(entry);
  }

  return result;
}
