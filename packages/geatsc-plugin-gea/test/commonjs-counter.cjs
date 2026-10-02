class Counter {
  /** @param {number} value */
  constructor(value) {
    this.value = value;
  }
  /** @param {number} amount */
  add(amount) {
    this.value += amount;
  }
}
/** @param {number} value */
function makeCounter(value) {
  return new Counter(value);
}
module.exports = makeCounter;
