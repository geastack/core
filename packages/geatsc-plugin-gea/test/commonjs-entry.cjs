const makeCounter = require("./commonjs-counter.cjs");
const sameCounter = require("./commonjs-counter.cjs");
if (makeCounter !== sameCounter) throw new Error("module identity changed");
const counter = makeCounter(40);
counter.add(2);
if (counter.value !== 42) throw new Error("module state was lost");
