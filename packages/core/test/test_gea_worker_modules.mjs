import assert from "node:assert/strict";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { discoverLocalImport } from "../scripts/gea-bundle-type-hints.mjs";
import {
  resolveWorkerModules,
  collectWorkerModuleSources,
} from "../scripts/gea-worker-modules.mjs";

test("module discovery follows nested native modules and skips unreachable tests", () => {
  const files = new Map([
    ["/app/main.ts", 'new Worker("./worker.ts", {type:"module"})'],
    ["/app/worker.ts", 'import "./audio.ts"'],
    ["/app/audio.ts", 'context.audioWorklet.addModule("./processor.ts")'],
    ["/app/processor.ts", 'registerProcessor("pcm", Processor)'],
    ["/app/unused.test.ts", "new Worker(dynamicURL)"],
  ]);
  const sources = collectWorkerModuleSources(
    "/app/main.ts",
    (file) => (file === "/app/worker.ts" ? ["/app/audio.ts"] : []),
    (file) => files.get(file),
  );
  assert.deepEqual([...sources.keys()].sort(), [
    "/app/audio.ts",
    "/app/main.ts",
    "/app/processor.ts",
    "/app/worker.ts",
  ]);
});

test("emitted JavaScript import names resolve back to TypeScript source", () => {
  const files = [];
  discoverLocalImport(
    "../runtime/audio-worklet.js",
    fileURLToPath(import.meta.url),
    files,
  );
  assert.deepEqual(files, [
    fileURLToPath(new URL("../runtime/audio-worklet.ts", import.meta.url)),
  ]);
});

test("worker URLs become registry keys without executing their modules on the UI", () => {
  const source =
    "const worker = new Worker(new URL('./agent.ts', import.meta.url), { type: 'module' })";
  const result = resolveWorkerModules(source, "/app/src/main.ts");
  assert.equal(result.modules.length, 1);
  assert.equal(result.modules[0].entry, "/app/src/agent.ts");
  assert.equal(result.modules[0].kind, "worker");
  assert.match(
    result.code,
    /new Worker\("file:\/\/\/app\/src\/agent.ts", \{ type: 'module' \}\)/,
  );
  assert.equal(
    result.modules[0].symbol,
    resolveWorkerModules(source, "/app/src/main.ts").modules[0].symbol,
  );
});

test("AudioWorklet modules preserve surrounding awaits and use their own entries", () => {
  const result = resolveWorkerModules(
    "await context.audioWorklet.addModule(new URL('./pcm.ts', import.meta.url))",
    "/app/src/main.ts",
  );
  assert.equal(result.modules[0].kind, "worklet");
  assert.equal(
    result.code,
    'await context.audioWorklet.addModule("file:///app/src/pcm.ts")',
  );
});

test("native discovery rejects URLs it cannot compile", () => {
  assert.throws(
    () => resolveWorkerModules("new Worker(getURL())", "/app/main.ts"),
    /statically resolvable/,
  );
  assert.throws(
    () =>
      resolveWorkerModules(
        'new Worker("https://example.com/worker.js")',
        "/app/main.ts",
      ),
    /relative module URL/,
  );
});

test("unrelated addModule calls and URL construction are unchanged", () => {
  const source =
    'registry.addModule("./module.ts"); const u = new URL("./data", import.meta.url)';
  assert.equal(resolveWorkerModules(source, "/app/main.ts").code, source);
});

test("type imports do not shadow Worker but local values do", () => {
  assert.equal(
    resolveWorkerModules(
      'import type { Worker } from "@geastack/core"; new Worker("./agent.ts")',
      "/app/main.ts",
    ).modules.length,
    1,
  );
  assert.equal(
    resolveWorkerModules(
      'import { type Worker } from "@geastack/core"; new Worker("./agent.ts")',
      "/app/main.ts",
    ).modules.length,
    1,
  );
  const source = "function f(Worker) { return new Worker(getURL()) }";
  assert.equal(resolveWorkerModules(source, "/app/main.ts").code, source);
});
