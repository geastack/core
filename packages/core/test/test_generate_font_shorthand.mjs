import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'

const source = fs.readFileSync(new URL('../scripts/generate-gea-embedded-fonts.mjs', import.meta.url), 'utf8')
const start = source.indexOf('function parseFontShorthand(')
const end = source.indexOf('function fontCssSelectors(', start)
const parse = vm.runInNewContext(`
const firstFontFamily = value => value.split(',')[0].trim().replace(/^['"]|['"]$/g, '')
const parseFontSizes = value => [value]
${source.slice(start, end)}
parseFontShorthand
`)

for (const [font, family, size] of [
  ['28px Maple', 'Maple', '28px'],
  ['italic 600 26px/1.2 "Montserrat SemiBold", sans-serif', 'Montserrat SemiBold', '26px'],
  ["bold 1.5rem / 20px 'Commissioner'", 'Commissioner', '1.5rem'],
  ['normal 2vmax Maple', 'Maple', '2vmax'],
]) {
  const result = parse(font)
  assert.equal(result.family, family)
  assert.equal(result.sizePxs[0], size)
}
for (const unsupported of ['inherit', 'caption', 'bold Maple', '28px']) {
  assert.equal(parse(unsupported), null)
}
console.log('Font shorthand atlas regression passed')

const selectorStart = source.indexOf('function fontCssSelectors(')
const selectorEnd = source.indexOf('function rememberFontTuple(', selectorStart)
const selectors = vm.runInNewContext(`${source.slice(selectorStart, selectorEnd)}; fontCssSelectors`)
assert.deepEqual(Array.from(selectors('.alarm-row > span, button:active')), ['.alarm-row > span', 'button:active'])
assert.deepEqual(Array.from(selectors('@font-face')), [])
