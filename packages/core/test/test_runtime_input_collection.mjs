import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const testDir = path.dirname(fileURLToPath(import.meta.url));
const core = path.resolve(testDir, "../../..");
const build = path.join(testDir, ".build/runtime-input-collection");
const compilerRuntime =
  process.env.GEA_COMPILER_RUNTIME ??
  path.resolve(core, "../compiler/src/targets/cpp/runtime");
mkdirSync(build, { recursive: true });

// Compile the production dispatcher verbatim. Only platform services are fakes;
// allocation-pressure collection below is the compiler's actual native runtime.
const source = readFileSync(path.join(testDir, "../runtime.cpp"), "utf8");
const start = source.indexOf("\nnamespace {\n");
const end = source.indexOf("\n}  // namespace", start);
assert.ok(start >= 0 && end > start);
const apiStart = source.indexOf("\nvoid Runtime::requestUiPause(", end);
const apiEnd = source.indexOf("\nvoid Runtime::runNativeBoot()", apiStart);
const runStart = source.indexOf(
  "\nvoid Runtime::run(const RuntimeOptions &options)",
);
const runEnd = source.lastIndexOf("\n}  // namespace gea::framework");
assert.ok(
  apiStart > end && apiEnd > apiStart && runStart > apiEnd && runEnd > runStart,
);
writeFileSync(
  path.join(build, "runtime_input_dispatch.inc"),
  source.slice(start, end) +
    "\n}\n" +
    source.slice(apiStart, apiEnd) +
    source.slice(runStart, runEnd),
);

for (const [variant, defines] of [
  ["full", []],
  ["canvas", ["-DGEA_EMBEDDED_DIRECT_CANVAS_CONTEXT=1"]],
  ["headless", ["-DGEA_EMBEDDED_NO_DISPLAY=1"]],
]) {
  // clang names a Windows executable with .exe; spawn needs the real name.
  const binary = path.join(
    build,
    "runtime-input-collection-" +
      variant +
      (process.platform === "win32" ? ".exe" : ""),
  );
  execFileSync(
    process.env.CXX || "clang++",
    [
      "-std=c++20",
      "-O1",
      "-g",
      "-fsanitize=address,undefined",
      "-pthread",
      ...defines,
      "-I" + compilerRuntime,
      "-I" + path.join(testDir, "../include"),
      "-I" + build,
      path.join(testDir, "test_runtime_input_collection.cpp"),
      "-o",
      binary,
    ],
    { stdio: "inherit", env: { ...process.env, TMPDIR: build } },
  );
  execFileSync(binary, { stdio: "inherit", timeout: 15000 });
}
