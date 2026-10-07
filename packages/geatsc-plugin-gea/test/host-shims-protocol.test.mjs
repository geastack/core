import assert from "node:assert/strict";
import test from "node:test";
import { fileURLToPath } from "node:url";
import ts from "typescript";

import { createGeaHostShims } from "../dist/host-shims.js";

// A listener parameter declared as any event interface index.d.ts publishes is
// handed the engine's one event struct. `Event` is declared by `onScroll` and by
// `EventTarget.addEventListener(type, listener: (event: Event) => void)`; without
// a carrier it lowered to a synthesized record the runtime listener adapter
// cannot build from a PointerEvent, and the app aborted on the first scroll.
test("every declared event parameter type lowers to the engine event, including Event", () => {
  const definitions = createGeaHostShims();
  for (const name of [
    "Event",
    "PointerEvent",
    "PressEvent",
    "PressEventArgument",
    "TouchEvent",
    "RotaryEvent",
    "InputEvent",
    "KeyEvent",
  ]) {
    assert.equal(
      definitions.nativeTypes?.[name],
      "gea::framework::events::PointerEvent",
      `${name} must lower to the engine event`,
    );
  }
  for (const member of ["type", "target", "currentTarget"]) {
    const rows = definitions.nativeMemberPropertyGetters?.[member] ?? [];
    assert.ok(
      rows.some((row) => row.receiverTypes?.includes("Event")),
      `Event.${member} must be a native member read`,
    );
  }
  for (const method of ["preventDefault", "stopPropagation"]) {
    const rows = definitions.nativeMemberMethods?.[method] ?? [];
    assert.ok(
      rows.some((row) => row.receiverTypes?.includes("Event")),
      `Event.${method}() must be a native member call`,
    );
  }
});

test("click callbacks receive an event with native methods and explicit numeric press metadata", () => {
  const declaration = fileURLToPath(
    new URL("../../core/index.d.ts", import.meta.url),
  );
  const program = ts.createProgram([declaration], {
    strict: true,
    skipLibCheck: true,
  });
  const checker = program.getTypeChecker();
  const source = program.getSourceFile(declaration);
  const exports = checker.getExportsOfModule(
    checker.getSymbolAtLocation(source),
  );
  const handler = exports.find((symbol) => symbol.name === "PressHandler");
  const signature = checker
    .getDeclaredTypeOfSymbol(handler)
    .getCallSignatures()[0];
  const argument = checker.getTypeOfSymbolAtLocation(
    signature.parameters[0],
    source,
  );

  assert.equal(
    checker.isTypeAssignableTo(argument, checker.getNumberType()),
    false,
    "a click event cannot be used as an arithmetic value",
  );
  for (const method of ["preventDefault", "stopPropagation"]) {
    const property = checker.getPropertyOfType(argument, method);
    assert.equal(
      checker.getTypeOfSymbolAtLocation(property, source).getCallSignatures()
        .length,
      1,
    );
  }
  const definitions = createGeaHostShims();
  for (const member of ["pressId", "pressValue"]) {
    const property = checker.getPropertyOfType(argument, member);
    assert.ok(
      checker.isTypeAssignableTo(
        checker.getTypeOfSymbolAtLocation(property, source),
        checker.getNumberType(),
      ),
    );
    assert.ok(
      definitions.nativeMemberPropertyGetters?.[member]?.some((row) =>
        row.receiverTypes?.includes("PressEvent"),
      ),
    );
  }
  for (const method of ["preventDefault", "stopPropagation"]) {
    assert.ok(
      definitions.nativeMemberMethods?.[method]?.some((row) =>
        row.receiverTypes?.includes("PressEvent"),
      ),
    );
  }
  assert.equal(
    definitions.nativeTypes?.PressEventTarget,
    "gea::framework::events::EventTarget",
  );
});

test("geolocation snapshot operations publish their non-throwing physical contract", () => {
  const definitions = createGeaHostShims();
  for (const namespace of ["Geolocation", "geolocation"]) {
    const methods = definitions.nativeNamespaceMethods?.[namespace];
    assert.equal(
      methods?.currentPosition?.noThrow,
      true,
      `${namespace}.currentPosition must be sealed noThrow`,
    );
    assert.equal(
      methods?.coords?.noThrow,
      true,
      `${namespace}.coords must be sealed noThrow`,
    );
  }
});

test("Profiler cycle clock is present in native and aliased host surfaces", () => {
  const definitions = createGeaHostShims();
  for (const namespace of ["Profiler", "__gea_Profiler"]) {
    assert.equal(
      definitions.hostNamespaceMethods?.[namespace]?.nowCycles,
      "gea::host::Profiler.nowCycles",
    );
    assert.ok(
      definitions.hostNamespaceNoThrowMethods?.[namespace]?.includes(
        "nowCycles",
      ),
    );
    assert.deepEqual(
      definitions.nativeNamespaceMethods?.[namespace]?.nowCycles,
      {
        emit: "gea::host::Profiler.nowCycles({args})",
        returnType: "double",
        noThrow: true,
      },
    );
  }
});
