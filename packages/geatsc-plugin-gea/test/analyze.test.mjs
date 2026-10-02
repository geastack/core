import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'

import { analyzeSourceHostBindings as analyzeAllFeatures } from '../dist/analyze.js'
// These assertions cover the pre-existing host/cache feature contract. Semantic
// CSS elimination has separate end-to-end assertions below.
function analyzeSourceHostBindings(entry) {
  const result = analyzeAllFeatures(entry)
  return { ...result, features: result.features.filter(feature => !feature.startsWith('css-') && !feature.startsWith('node-') && !feature.startsWith('renderer-occlusion-') && !feature.startsWith('runtime-')) }
}

function app(t, files) {
  const root = path.resolve('/virtual-gea-plugin-analyze')
  const sources = new Map(Object.entries(files).map(([name, text]) => [path.join(root, name), text]))
  t.mock.method(fs, 'existsSync', (file) => sources.has(file))
  t.mock.method(fs, 'statSync', () => ({ isFile: () => true }))
  t.mock.method(fs, 'readFileSync', (file) => sources.get(file))
  return path.join(root, 'index.tsx')
}

test('an https literal reached through a relative import reports the https feature', (t) => {
  const entry = app(t, {
    'index.tsx': "import { Display, mount } from '@geastack/core'\nimport { weather } from './stores/WeatherStore'\n// see https://example.com for the API\nmount(weather)\n",
    'stores/WeatherStore.ts': "import { Store, WiFi } from '@geastack/core'\nexport class WeatherStore extends Store {\n  fetchInFlight = 0\n  async load(id: number) {\n    const response = await fetch('https://api.open-meteo.com/v1/forecast?id=' + id)\n    return response.json()\n  }\n}\nexport const weather = new WeatherStore()\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['display', 'fetch', 'wifi'], features: ['https', 'renderer-analysis-v1'] })
})

test('a comment citing an https URL does not link TLS, and a member fetch is not the host global', (t) => {
  const entry = app(t, {
    'index.tsx': "import { Display } from '@geastack/core'\n// Source: https://github.com/example/example\nclass Loader { fetch(url: string) { return url } }\nnew Loader().fetch('http://192.168.1.2/tile')\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['display'], features: ['renderer-analysis-v1'] })
})

test('a WebSocket constructor and a wss literal both bring the network stack', (t) => {
  const entry = app(t, {
    'index.tsx': "const socket = new WebSocket(`wss://${host}/stream`)\n",
  })
  assert.deepEqual(analyzeSourceHostBindings(entry), { bindings: ['websocket'], features: ['https', 'renderer-analysis-v1'] })
})

test('rounded CSS retains circle spans even when the canvas only draws video', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'; const frame = <canvas style={{ width: width, borderRadius: 16 }} />;
      ctx.clearRect(0, 0, 240, 240); ctx.drawImage(image, 0, 0); pixels[i] = value;`,
    'style.css': 'canvas { border-radius: 16px; background: #123; }',
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-circles'])
})

test('CSS imports recurse through quoted and unquoted url imports', (t) => {
  const entry = app(t, {
    'index.tsx': `import './styles/base.css'`,
    'styles/base.css': `@import 'colors.css'; @import url(motion.css);`,
    'styles/colors.css': '.card { background: linear-gradient(red, blue); }',
    'styles/motion.css': `@import url('base.css'); @keyframes spin { to { transform: rotate(1turn); } }`,
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-linear-gradients', 'renderer-transforms'])
})

test('inline styles, assignments and canvas shapes retain only their own features', (t) => {
  const entry = app(t, {
    'index.tsx': `const look = { backgroundImage: 'radial-gradient(red, blue)' }; const card = <div style={look} />;
      element.style.transform = rotation; ctx.arc(2, 2, 1, 0, 6.28);`,
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-circles', 'renderer-radial-gradients', 'renderer-transforms'])
})

for (const source of [
  'const card = <div style={theme} />',
  'element.style.cssText = cssFromNetwork',
  'element.style[property] = value',
  'element.style.setProperty(property, value)',
  `element.setAttribute('style', styles)`,
  'Object.assign(element.style, theme)',
  'const styles = element.style; styles[property] = value',
  'const styles = element.style; Object.assign(styles, theme)',
  'sheet.insertRule(rule)',
  'const card = <div {...props} />',
  'const card = <div style={{ ...theme }} />',
]) test(`dynamic style conservatively retains renderer features: ${source}`, (t) => {
  const entry = app(t, { 'index.tsx': source })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms'])
})

test('dynamic backgrounds retain gradients and a computed canvas call retains shapes too', (t) => {
  const entry = app(t, { 'index.tsx': `ctx[operation](...args); const card = <div style={{ background: theme }} />` })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-circles', 'renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms'])
})

test('unresolved style imports cannot certify renderer features as absent', (t) => {
  const entry = app(t, { 'index.tsx': `import './not-generated-yet.css'` })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-circles', 'renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms'])
})

test('NodeNext imports follow the original TypeScript module', (t) => {
  const entry = app(t, { 'index.tsx': `export { look } from './look.js'`, 'look.ts': `export const look = { transform: 'rotate(12deg)' }` })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-transforms'])
})

test('opaque package imports retain rendering features conservatively', (t) => {
  const entry = app(t, { 'index.tsx': `import { Card } from 'custom-components'; const card = <Card />` })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-circles', 'renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms'])
})

// Existing family snapshots keep their original scope; complete v16 proofs
// have independent positive/negative cases below.
const baseStyleFamilies = ['css-flex-direction', 'css-justify-content', 'css-align-items', 'css-box-sizing', 'css-margin-auto', 'css-line-height-multiplier', 'css-width-expressions', 'css-min-height', 'css-max-width', 'css-active-background']
const defaultStyleFamilies = ['css-margins', 'css-padding', 'css-flex-factors', 'css-gap', 'css-border-widths', 'css-border-colors', 'css-font-weight', 'css-text-align', 'css-white-space', 'css-text-overflow']
function cssFeatures(entry) { return analyzeAllFeatures(entry).features.filter(feature => feature.startsWith('css-') && !feature.startsWith('css-range') && !feature.startsWith('css-circle-cache-') && !['css-storage-v1', 'css-line-height', 'css-display-explicit', 'css-width-percent', 'css-height-percent', 'css-custom-properties', 'css-text-alpha', 'css-border-alpha', 'css-pseudo-elements', ...baseStyleFamilies, ...defaultStyleFamilies].includes(feature)) }

test('CSS engine capabilities are inferred without app opt-ins', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'; const node = <div style={{ width: liveWidth }} />`,
    'style.css': `.card { box-sizing: border-box; display: flex; width: calc(100% - 20px); border: 0; }`,
  })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19'])
})

test('CSS shorthand and selectors retain only reachable semantic families', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'`,
    'style.css': `.card { border: 3px inset blue; display: grid; } @keyframes spin { to { transform: rotate(1turn); } }`,
  })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-animations', 'css-border-relief', 'css-grid', 'css-transforms'])
})

test('dynamic style names and unknown modules retain all semantics automatically', (t) => {
  const entry = app(t, { 'index.tsx': `element.style[property] = value` })
  assert.deepEqual(cssFeatures(entry), ['css-align-content', 'css-align-self', 'css-analysis-v19', 'css-animations', 'css-aspect-ratio', 'css-axis-gap', 'css-background-layers', 'css-blink', 'css-border-relief', 'css-box-expressions', 'css-box-shadow', 'css-containment', 'css-corner-radius', 'css-custom-property-lengths', 'css-filters', 'css-first-line', 'css-flex-basis', 'css-flex-basis-expressions', 'css-flex-line-count', 'css-flex-wrap', 'css-floats', 'css-grid', 'css-height-expressions', 'css-image-fit', 'css-justify-items', 'css-justify-self', 'css-line-height-expressions', 'css-margin-trim', 'css-mask', 'css-max-height', 'css-min-width', 'css-opacity', 'css-order', 'css-overflow-axes', 'css-percent-gap', 'css-percent-radius', 'css-pointer-events', 'css-position-bottom', 'css-position-bottom-percent', 'css-position-left', 'css-position-left-percent', 'css-position-right', 'css-position-right-percent', 'css-position-top', 'css-position-top-percent', 'css-scrolling', 'css-side-borders', 'css-text-decoration', 'css-text-transform', 'css-transforms', 'css-visibility', 'css-writing-mode', 'css-z-index'])
})

test('dynamic values retain their family without retaining unrelated CSS', (t) => {
  const entry = app(t, { 'index.tsx': `const node = <div style={{ display: mode, borderStyle: style, width: liveWidth }} />` })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-border-relief', 'css-grid', 'css-side-borders'])
})


test('shadowed constants cannot incorrectly remove a border capability', (t) => {
  const entry = app(t, { 'index.tsx': `function f() { const border = '3px inset blue'; return <div style={{border}}/> } const border = '0';` })
  assert.ok(cssFeatures(entry).includes('css-border-relief'))
})

test('escaping style objects and aliased style setters preserve semantic support', (t) => {
  const entry = app(t, { 'index.tsx': `mutate(node.style); const setter = node.style.setProperty; setter(name, value);` })
  assert.deepEqual(cssFeatures(entry), ['css-align-content', 'css-align-self', 'css-analysis-v19', 'css-animations', 'css-aspect-ratio', 'css-axis-gap', 'css-background-layers', 'css-blink', 'css-border-relief', 'css-box-expressions', 'css-box-shadow', 'css-containment', 'css-corner-radius', 'css-custom-property-lengths', 'css-filters', 'css-first-line', 'css-flex-basis', 'css-flex-basis-expressions', 'css-flex-line-count', 'css-flex-wrap', 'css-floats', 'css-grid', 'css-height-expressions', 'css-image-fit', 'css-justify-items', 'css-justify-self', 'css-line-height-expressions', 'css-margin-trim', 'css-mask', 'css-max-height', 'css-min-width', 'css-opacity', 'css-order', 'css-overflow-axes', 'css-percent-gap', 'css-percent-radius', 'css-pointer-events', 'css-position-bottom', 'css-position-bottom-percent', 'css-position-left', 'css-position-left-percent', 'css-position-right', 'css-position-right-percent', 'css-position-top', 'css-position-top-percent', 'css-scrolling', 'css-side-borders', 'css-text-decoration', 'css-text-transform', 'css-transforms', 'css-visibility', 'css-writing-mode', 'css-z-index'])
})

for (const source of [
  `const display = 'flex'; function card(display) { return <div style={{display}}/> }`,
  `const display = 'flex'; function card({display}) { return <div style={{display}}/> }`,
]) test(`shadowing a literal with a parameter preserves grid: ${source}`, (t) => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-grid'))
})

for (const source of [
  `const styles = {}; styles[key] = value; const card = <div style={styles}/>`,
  `element.innerHTML = markup`,
  `const sheet = document.createElement('style'); sheet.textContent = css`,
]) test(`opaque style construction preserves semantic support: ${source}`, (t) => {
  assert.deepEqual(cssFeatures(app(t, { 'index.tsx': source })), ['css-align-content', 'css-align-self', 'css-analysis-v19', 'css-animations', 'css-aspect-ratio', 'css-axis-gap', 'css-background-layers', 'css-blink', 'css-border-relief', 'css-box-expressions', 'css-box-shadow', 'css-containment', 'css-corner-radius', 'css-custom-property-lengths', 'css-filters', 'css-first-line', 'css-flex-basis', 'css-flex-basis-expressions', 'css-flex-line-count', 'css-flex-wrap', 'css-floats', 'css-grid', 'css-height-expressions', 'css-image-fit', 'css-justify-items', 'css-justify-self', 'css-line-height-expressions', 'css-margin-trim', 'css-mask', 'css-max-height', 'css-min-width', 'css-opacity', 'css-order', 'css-overflow-axes', 'css-percent-gap', 'css-percent-radius', 'css-pointer-events', 'css-position-bottom', 'css-position-bottom-percent', 'css-position-left', 'css-position-left-percent', 'css-position-right', 'css-position-right-percent', 'css-position-top', 'css-position-top-percent', 'css-scrolling', 'css-side-borders', 'css-text-decoration', 'css-text-transform', 'css-transforms', 'css-visibility', 'css-writing-mode', 'css-z-index'])
})

for (const source of [`const card = <div dir="rtl"/>`, `element.dir = direction`, `element.setAttribute('dir', direction)`])
  test(`HTML direction preserves writing-mode support: ${source}`, (t) => {
    assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-writing-mode'))
  })


test('CSS property escapes retain semantics while icon escapes add no features', (t) => {
  const entry = app(t, { 'index.tsx': `import './style.css'`, 'style.css': String.raw`.icon { content: "\f001"; } .card { tr\61 nsform: rotate(10deg); }` })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-transforms'])
})

for (const mutation of ['mutate(styles)', 'const alias = styles; mutate(alias)', 'mutate({ appearance: styles })', 'const alias = styles; alias[key] = value'])
  test(`escaped style objects retain support: ${mutation}`, (t) => {
    const entry = app(t, { 'index.tsx': `const styles = { display: 'flex' }; ${mutation}; const card = <div style={styles}/>` })
    assert.ok(cssFeatures(entry).includes('css-grid'))
    assert.ok(cssFeatures(entry).includes('css-transforms'))
  })

for (const source of [
  'const node = <div style={{ blinkInterval: interval, order: priority }} />',
  'element.style.blinkInterval = interval; element.style.order = priority',
]) test(`individual style fields retain direct storage when used: ${source}`, (t) => {
  const entry = app(t, { 'index.tsx': source })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-blink', 'css-order'])
})

test('CSS order retains storage and unknown property names retain blink too', (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': '.card { order: -2; }' })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-order'])
})


test('literal pixel radii and gaps omit percentage storage automatically', (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': '.card { border-radius: 3px 2em; gap: 0 12px; }' })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-axis-gap', 'css-corner-radius'])
})
for (const value of ['25%', 'calc(25% + 2px)', 'var(--size)'])
  test(`percentage radius/gap representation remains for ${value}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.card { border-top-left-radius: ${value}; column-gap: ${value}; }` })
    assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-axis-gap', 'css-corner-radius', 'css-percent-gap', 'css-percent-radius'])
  })
test('dynamic radius/gap values retain the percentage representation', (t) => {
  const entry = app(t, { 'index.tsx': 'const node = <div style={{ borderRadius: radius, rowGap: gap }} />' })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-axis-gap', 'css-corner-radius', 'css-percent-gap', 'css-percent-radius'])
})

for (const [property, feature] of [
  ['opacity', 'css-opacity'], ['objectFit', 'css-image-fit'],
  ['textDecorationLine', 'css-text-decoration'], ['textTransform', 'css-text-transform'],
  ['visibility', 'css-visibility'], ['pointerEvents', 'css-pointer-events'],
  ['maskImage', 'css-mask'], ['-webkit-mask-image', 'css-mask'],
]) test(`optional property storage is retained automatically: ${property}`, t => {
  const entry = app(t, { 'index.tsx': `const element = <div style={{ '${property}': dynamicValue }} />` })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', feature, ...(property === 'placeSelf' ? ['css-justify-self'] : [])].sort())
})
for (const source of [
  `const element = <div data-anim="opacity" />`,
  `element.setAttribute('data-anim', kind)`,
  `element.setAttribute(attribute, value)`,
  `element.dataset.anim = kind`,
  `const props = { 'data-anim': kind }; const element = <div {...props} />`,
]) test(`attribute-driven animations retain style semantics: ${source}`, t => {
  const result = cssFeatures(app(t, { 'index.tsx': source }))
  assert.ok(result.includes('css-opacity'))
  assert.ok(result.includes('css-transforms'))
})
test('keyframes preserve opacity and text presentation fields', t => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': '@keyframes pulse { from { opacity: 0; visibility: hidden; } to { opacity: 1; visibility: visible; } }' })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-animations', 'css-opacity', 'css-visibility'])
})

for (const [property, feature] of [
  ['filter', 'css-filters'], ['boxShadow', 'css-box-shadow'],
  ['flexFlow', 'css-flex-wrap'], ['placeItems', 'css-justify-items'],
  ['placeContent', 'css-align-content'], ['placeSelf', 'css-align-self'],
  ['minWidth', 'css-min-width'], ['minInlineSize', 'css-min-width'],
]) test(`layout/effect storage is retained for ${property}`, t => {
  const entry = app(t, { 'index.tsx': `const node = <div style={{ ${property}: value }} />` })
  assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', feature, ...(property === 'placeSelf' ? ['css-justify-self'] : [])].sort())
})
for (const value of ['10px', '25%', '0', 'auto', 'inherit', '20px !important'])
  test(`simple height ${value} needs no deferred expression`, t => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.node { height: ${value}; }` })
    assert.deepEqual(cssFeatures(entry), ['css-analysis-v19'])
  })
for (const value of ['calc(100% - 10px)', 'var(--height)', 'max-content', 'fit-content', '10vh', '2em'])
  test(`height ${value} retains deferred representation`, t => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.node { height: ${value}; }` })
    assert.deepEqual(cssFeatures(entry), ['css-analysis-v19', 'css-height-expressions'])
  })
test('dynamic heights and logical sizes retain height expressions', t => {
  for (const source of ['const node = <div style={{height: size}} />', 'element.style.blockSize = size']) {
    assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-height-expressions'))
  }
})

for (const [property, feature] of [
  ['zIndex', 'css-z-index'], ['aspectRatio', 'css-aspect-ratio'],
  ['marginTrim', 'css-margin-trim'], ['contain', 'css-containment'],
  ['contentVisibility', 'css-containment'], ['justifySelf', 'css-justify-self'],
  ['flexLineCount', 'css-flex-line-count'],
]) test(`optional sizing/stacking storage follows ${property}`, t => {
  assert.deepEqual(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ ${property}: value }} />` })), ['css-analysis-v19', feature].sort())
})
for (const value of ['0', '4px', '3px 7px', '0 auto', '-1.5px', 'inherit', '1px 2px 3px 4px !important'])
  test(`fixed edges ${value} need no deferred storage`, t => {
    assert.deepEqual(cssFeatures(app(t, { 'index.tsx': "import './style.css'", 'style.css': `.node { margin: ${value}; padding: ${value}; }` })), ['css-analysis-v19'])
  })
for (const value of ['10%', '0 10%', '2em', 'calc(10% - 2px)', 'var(--edge)', 'env(safe-area-inset-top)'])
  test(`relative edges ${value} retain deferred storage`, t => {
    assert.deepEqual(cssFeatures(app(t, { 'index.tsx': "import './style.css'", 'style.css': `.node { padding-inline: ${value}; margin-block-start: ${value}; }` })), ['css-analysis-v19', 'css-box-expressions'])
  })
test('dynamic edges retain deferred storage', t => {
  assert.deepEqual(cssFeatures(app(t, { 'index.tsx': 'element.style.marginLeft = edge' })), ['css-analysis-v19', 'css-box-expressions'])
})


test('literal colour variables across discovered files do not reserve gradient caches', (t) => {
  const entry = app(t, {
    'index.tsx': `import './use.css'; import './colors.css'`,
    'use.css': '.card { background: var(--paint); } .other { background: var(--Paint, #1234); }',
    'colors.css': '.a { --paint: #fff; --Paint: #12345678; } .b { --paint: #abcdef !important; }',
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1'])
})

for (const [label, declarations, use] of [
  ['undefined name', '', 'var(--paint)'],
  ['case-sensitive names', '--Paint: #123', 'var(--paint)'],
  ['gradient override', '--paint: #123; --paint: linear-gradient(red, blue)', 'var(--paint)'],
  ['variable alias', '--paint: var(--other); --other: #123', 'var(--paint)'],
  ['opaque value', '--paint: env(theme)', 'var(--paint)'],
  ['complex background', '--paint: #123', 'var(--paint) var(--size)'],
  ['gradient fallback', '--paint: #123', 'var(--paint, radial-gradient(red, blue))'],
  ['nested fallback', '--paint: #123; --fallback: #456', 'var(--paint, var(--fallback))'],
]) test(`variable gradient analysis retains caches for ${label}`, (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'`,
    'style.css': `.card { ${declarations}; background: ${use}; }`,
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-linear-gradients', 'renderer-radial-gradients'])
})

for (const mutation of [
  `element.style.setProperty('--paint', liveColor)`,
  `element.style['--paint'] = liveColor`,
  `const styles = element.style; styles[name] = liveColor`,
  `const style = { '--paint': '#123' }; mutate(style); const node = <div style={style} />`,
  'const sheet = css`body { --paint: ${theme}; }`',
  `CSS.registerProperty({ name: '--paint', initialValue: theme })`,
]) test(`unknown custom property writes retain variable caches: ${mutation}`, (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'; ${mutation}`,
    'style.css': '.card { --paint: #123; background: var(--paint); }',
  })
  const features = analyzeSourceHostBindings(entry).features
  assert.ok(features.includes('renderer-linear-gradients'))
  assert.ok(features.includes('renderer-radial-gradients'))
})

test('literal custom property assignments keep the proof, independent of import order', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'; import './colors'; element.style.setProperty('--paint', '#abcdef')`,
    'style.css': '.card { background: var(--paint, transparent); }',
    'colors.tsx': `const card = <div style={{ '--paint': '#123' }} />`,
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1'])
})

test('escaped colour variable declarations are decoded before matching', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'`,
    'style.css': String.raw`.card { --p\61 int: #123; background: var(--paint); }`,
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1'])
})


test('standard CSS property names stay case-insensitive while variable names stay exact', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'`,
    'style.css': '.card { --paint: #123; BACKGROUND: var(--Unknown); }',
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1', 'renderer-linear-gradients', 'renderer-radial-gradients'])
})


for (const value of ['0', '12px', '1.5px', '3em', '25%', '4px 4px', 'initial', 'inherit', 'unset'])
  test(`uniform literal radii/gaps share their stored value: ${value}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.card { border-radius: ${value}; gap: ${value}; }` })
    const features = cssFeatures(entry)
    assert.ok(!features.includes('css-corner-radius'))
    assert.ok(!features.includes('css-axis-gap'))
    if (value.includes('%')) assert.ok(features.includes('css-percent-radius') && features.includes('css-percent-gap'))
  })

for (const [property, value, feature] of [
  ['border-radius', '3px 4px', 'css-corner-radius'],
  ['border-radius', '4px / 8px', 'css-corner-radius'],
  ['border-radius', 'var(--radius)', 'css-corner-radius'],
  ['border-top-left-radius', '4px', 'css-corner-radius'],
  ['border-start-end-radius', '4px', 'css-corner-radius'],
  ['gap', '3px 4px', 'css-axis-gap'],
  ['gap', 'calc(10% + 2px)', 'css-axis-gap'],
  ['gap', 'var(--gap)', 'css-axis-gap'],
  ['row-gap', '3px', 'css-axis-gap'],
  ['grid-column-gap', '3px', 'css-axis-gap'],
]) test(`independent storage retained for ${property}: ${value}`, (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.card { ${property}: ${value}; }` })
  assert.ok(cssFeatures(entry).includes(feature))
})

test('dynamic corner and axis values keep their independent storage', (t) => {
  const entry = app(t, { 'index.tsx': `const n = <div style={{ borderRadius: liveRadius, gap: liveGap }} />` })
  const features = cssFeatures(entry)
  assert.ok(features.includes('css-corner-radius'))
  assert.ok(features.includes('css-axis-gap'))
})


for (const selector of ['p::first-line', 'p:first-line', 'P::FIRST-LINE', String.raw`p::first-\6c ine`])
  test(`first-line storage follows reachable selectors: ${selector}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `${selector} { background: #123; }` })
    assert.ok(cssFeatures(entry).includes('css-first-line'))
  })

test('ordinary inline text and first-letter rules do not enable first-line records', (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'; const n = <span>text</span>", 'style.css': 'p::first-letter { color: red; }' })
  assert.ok(!cssFeatures(entry).includes('css-first-line'))
})

for (const source of [
  `sheet.insertRule('p::first-line { background: #123; }')`,
  `sheet.insertRule(runtimeRule)`,
  'const rule = css`p::${pseudo} { color: red; }`',
]) test(`dynamic sheets preserve first-line support: ${source}`, (t) => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-first-line'))
})

for (const declaration of ['border: 2px solid #123', 'border-width: 2px 2px', 'border-style: solid solid solid', 'border-color: var(--Color)', 'background: var(--Color, #456)', 'line-height: 1.15', 'line-height: 14px', 'font: inherit']) {
  test(`uniform/flat styles omit unused rare families: ${declaration}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `:root { --Color: #123; } .x { ${declaration}; }` })
    const features = cssFeatures(entry)
    for (const feature of ['css-side-borders', 'css-background-layers', 'css-line-height-expressions']) assert.ok(!features.includes(feature), feature)
  })
}
for (const [declaration, feature] of [
  ['border-left: 2px solid #123', 'css-side-borders'], ['border-inline-width: 2px', 'css-side-borders'],
  ['border-width: 2px 3px', 'css-side-borders'], ['border-style: solid none', 'css-side-borders'],
  ['border-color: #123 #456', 'css-side-borders'], ['border-color: var(--unknown)', 'css-side-borders'],
  ['background-image: none, none', 'css-background-layers'], ['background-clip: content-box', 'css-background-layers'],
  ['background: linear-gradient(#123, #456)', 'css-background-layers'], ['background: var(--unknown)', 'css-background-layers'],
  ['line-height: calc(100% + 1px)', 'css-line-height-expressions'], ['line-height: 1.5em', 'css-line-height-expressions'],
  ['font: 14px/120% serif', 'css-line-height-expressions'],
]) test(`rare storage remains for ${declaration}`, (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.x { ${declaration}; }` })
  assert.ok(cssFeatures(entry).includes(feature))
})
for (const definition of ['--Color: var(--alias); --alias: #123', '--Color: #123; --Color: #123 #456', '--color: #123']) {
  test(`variable proof retains support for alias, nonuniform or wrong case: ${definition}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': `.x { ${definition}; background: var(--Color); border-color: var(--Color); }` })
    const features = cssFeatures(entry)
    assert.ok(features.includes('css-side-borders'))
    assert.ok(features.includes('css-background-layers'))
  })
}
for (const [property, feature] of [['borderWidth', 'css-side-borders'], ['borderColor', 'css-side-borders'], ['background', 'css-background-layers'], ['lineHeight', 'css-line-height-expressions']]) {
  test(`dynamic ${property} retains its storage`, (t) => assert.ok(cssFeatures(app(t, { 'index.tsx': `node.style.${property} = value` })).includes(feature)))
}

for (const value of ['visible', 'hidden', 'clip', 'hidden clip', 'clip visible !important', 'initial', 'inherit']) test(`non-scrolling overflow ${value} omits scroll state`, t => {
  assert.ok(!cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ overflow: '${value}' }} />` })).includes('css-scrolling'))
})
for (const value of ['auto', 'scroll', 'overlay', 'hidden auto', 'var(--overflow)', 'calc(1px)']) test(`overflow ${value} retains scroll state`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ overflow: '${value}' }} />` })).includes('css-scrolling'))
})
for (const source of [
  'const node = <virtual-list />', 'const node = <input />', 'const node = <textarea />', 'const node = <select />',
  "document.createElement('virtual-list')", 'tree.createVirtualList()', 'node.style.overflowY = value',
  'document.createElement(tag)', 'const create = document.createElement; create(tag)',
  'const {createElement: create} = document; create(tag)', 'document.createElementNS(namespace, tag)',
  "const node = <div style={{ backgroundAttachment: 'local' }} />",
  "const node = <div style={{ background: 'url(pic.png) local' }} />",
]) test(`native or opaque scroll usage retains state: ${source}`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-scrolling'))
})
test('literal color variables and clipped axes require no scroll metadata', t => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': ':root { --panel: #123; } div { background: var(--panel); overflow: hidden clip; }' })
  assert.ok(!cssFeatures(entry).includes('css-scrolling'))
})

for (const value of ['visible hidden', 'hidden visible', 'visible hidden !important']) test(`coupled overflow axes ${value} retain scrolling`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ overflow: '${value}' }} />` })).includes('css-scrolling'))
})
for (const property of ['overflowX', 'overflowY', 'overflowInline', 'overflowBlock']) test(`cascading ${property} retains coupled-axis scrolling`, t => {
  for (const value of ['hidden', 'visible', 'clip', 'inherit']) assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ ${property}: '${value}' }} />` })).includes('css-scrolling'))
})

for (const value of ['none', 'auto', '1', '1 0', '1 0 20px', '0 1 auto']) test(`fixed flex ${value} needs no deferred basis`, t => {
  assert.ok(!cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ flex: '${value}' }} />` })).includes('css-flex-basis-expressions'))
})
for (const value of ['20%', 'calc(50% - 2px)', '2em', 'var(--basis)']) test(`deferred flex basis ${value} retains storage`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ flexBasis: '${value}' }} />` })).includes('css-flex-basis-expressions'))
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ flex: '1 0 ${value}' }} />` })).includes('css-flex-basis-expressions'))
})
test('dynamic flex declarations retain deferred storage', t => {
  for (const property of ['flex', 'flexBasis']) assert.ok(cssFeatures(app(t, { 'index.tsx': `const node = <div style={{ ${property}: value }} />` })).includes('css-flex-basis-expressions'))
})
test('proven color custom properties omit length caches', t => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': ':root { --one: #123; --two: #123456; --three: transparent; } .box { background: var(--one); }' })
  assert.ok(!cssFeatures(entry).includes('css-custom-property-lengths'))
})
for (const value of ['1', '20px', '2em', 'var(--other)', 'calc(10px + 20px)']) test(`custom property ${value} retains length cache`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': "import './style.css'", 'style.css': `:root { --value: ${value}; }` })).includes('css-custom-property-lengths'))
})

for (const value of ['"20px"', "'123'", String.raw`'ABCDEFG#0123456789-\2014'`, "'hello' !important"]) test(`quoted custom property ${value} needs no length cache`, t => {
  assert.ok(!cssFeatures(app(t, { 'index.tsx': "import './style.css'", 'style.css': `:root { --value: ${value}; }` })).includes('css-custom-property-lengths'))
})
test('quoted custom properties do not hide numeric definitions elsewhere', t => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': ':root { --value: "20px"; } .other { --value: 20px; }' })
  assert.ok(cssFeatures(entry).includes('css-custom-property-lengths'))
})

test('decoded escaped quotes conservatively retain custom-property length support', t => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': String.raw`:root { --value: "a\"b"; }` })
  assert.ok(cssFeatures(entry).includes('css-custom-property-lengths'))
})

for (const value of ['none', 'auto', '1', '1 0', 'initial']) test(`implicit flex basis ${value} needs no scalar field`, t => {
  assert.ok(!cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ flex: '${value}' }} />` })).includes('css-flex-basis'))
})
for (const value of ['20px', '20%', 'auto', 'calc(50% - 2px)']) test(`explicit flex basis ${value} retains scalar support`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ flexBasis: '${value}' }} />` })).includes('css-flex-basis'))
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ flex: '1 0 ${value}' }} />` })).includes('css-flex-basis'))
})
for (const property of ['maxHeight', 'maxBlockSize', 'maxInlineSize']) test(`${property} retains maximum-height support`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${property}: value }} />` })).includes('css-max-height'))
})
for (const value of ['hidden', 'clip', 'visible', 'auto', 'scroll', 'hidden hidden', 'inherit']) test(`uniform overflow ${value} needs one field`, t => {
  assert.ok(!cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ overflow: '${value}' }} />` })).includes('css-overflow-axes'))
})
for (const value of ['visible hidden', 'hidden clip', 'clip visible', 'var(--axes)']) test(`distinct or unknown overflow ${value} retains axis fields`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ overflow: '${value}' }} />` })).includes('css-overflow-axes'))
})
test('native inputs and lists retain independent overflow axes', t => {
  for (const tag of ['input', 'virtual-list', 'textarea']) assert.ok(cssFeatures(app(t, { 'index.tsx': `const n = <${tag} />` })).includes('css-overflow-axes'))
})

function rangeFeatures(entry) { return analyzeAllFeatures(entry).features.filter(feature => feature.startsWith('css-range')) }

test('range proof keeps raw values separate from CSS pixels and closes over inheritance', (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'", 'style.css': '.a { padding: 0 9px; gap: 7px; border: 0; border-width: 2px; border-radius: 65px; font-size: 52px; line-height: 1.15; flex: 2 3 10px; } .b { padding: inherit; font: inherit; line-height: 56px; }' })
  const features = rangeFeatures(entry)
  for (const feature of ['css-ranges-v1', 'css-range-padding-px-9', 'css-range-gap-px-7', 'css-range-border-px-2', 'css-range-radius-px-65', 'css-range-font-px-52', 'css-range-line-height-px-104', 'css-range-flex-raw-3']) assert.ok(features.includes(feature), feature)
  assert.ok(!features.includes('css-ranges-unknown'))
})

for (const [property, value, family] of [
  ['padding', '10%', 'padding'], ['paddingInline', '1em', 'padding'], ['gap', 'var(--gap)', 'gap'],
  ['border', 'var(--stroke) solid red', 'border'], ['borderWidth', '-1px', 'border'],
  ['borderRadius', '50%', 'radius'], ['fontSize', '2em', 'font'],
  ['lineHeight', 'calc(1em + 2px)', 'line-height'], ['flex', '1 var(--shrink)', 'flex'],
  ['padding', '1px 2px 3px 4px 5px', 'padding'], ['fontSize', '1e20px', 'font'],
]) test(`range proof retains wide ${property}: ${value}`, (t) => {
  const features = rangeFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${property}: '${value}' }} />` }))
  assert.ok(!features.some(feature => feature.startsWith(`css-range-${family}-`)))
})

for (const source of [
  `node.style.padding = value`, `node.style.setProperty('padding', value)`,
  `const look = { padding: '2px' }; mutate(look); const n = <div style={look} />`,
  `import { Button } from '@geastack/core'; const n = <Button />`,
  `import { Display as Screen } from '@geastack/core'; Screen.setDevicePixelRatio(8)`,
  `host['setDevicePixelRatio'](8)`, `const n = <input />`, `__gea_Button.create()`,
  `node.style.fontSize(value)`, `const n = <div style={{ transition: 'padding 1s cubic-bezier(0, 2, 1, 2)' }} />`,
]) test(`unknown runtime bounds cannot narrow padding: ${source}`, (t) => {
  const features = rangeFeatures(app(t, { 'index.tsx': source }))
  assert.ok(features.includes('css-ranges-unknown') || !features.some(feature => feature.startsWith('css-range-padding-')))
})

test('unknown font sizes invalidate inherited multiplier line-height bounds', (t) => {
  const features = rangeFeatures(app(t, { 'index.tsx': `const n = <div style={{ fontSize: size, lineHeight: 1.2 }} />` }))
  assert.ok(!features.some(feature => feature.startsWith('css-range-line-height-')))
})

test('unknown imports and keyframe overshoot do not create a range proof', (t) => {
  const entry = app(t, { 'index.tsx': "import './style.css'; import { C } from 'opaque'", 'style.css': '@keyframes pulse { from { padding: 0; } to { padding: 40px; } }' })
  assert.ok(rangeFeatures(entry).includes('css-ranges-unknown'))
})

for (const imports of [
  `import './style.css'; import { Component } from '@geastack/core'`,
  `import './style.css'; export { helper } from './helper'`,
  `require('./style.css'); const value = 'from ignored';`,
  `import styles = require('./style.css')`,
]) test(`source discovery retains leading stylesheet: ${imports}`, (t) => {
  const entry = app(t, { 'index.tsx': imports, 'style.css': '.root { border-radius: 65px; transform: scale(2); }', 'helper.ts': 'export const helper = 1' })
  const features = analyzeAllFeatures(entry).features
  assert.ok(features.includes('css-transforms'))
  assert.ok(features.includes('renderer-transforms'))
  assert.ok(features.includes('css-range-radius-px-65'))
})

test('computed require retains all semantics and cannot establish bounded storage', (t) => {
  const features = analyzeAllFeatures(app(t, { 'index.tsx': 'const widget = require(name)' })).features
  assert.ok(features.includes('css-transforms') && features.includes('css-ranges-unknown'))
})


const nativeNodes = entry => analyzeAllFeatures(entry).features.filter(feature => ['node-analysis-v1', 'node-images', 'node-inputs'].includes(feature))
test('basic DOM and a custom button keyboard need no native image/input payloads', t => {
  const entry = app(t, { 'index.tsx': `import { Component, type GeaElement } from '@geastack/core';
    export class App extends Component { template() { return <div><button>Key</button><span>Value</span></div> } }` })
  assert.deepEqual(nativeNodes(entry), ['node-analysis-v1'])
})
for (const source of [`<img src="x" />`, `document.createElement('img')`, `document.createElementNS('x', 'image')`, `new Image()`, `<svg:image />`, `document.createElement('IMG')`, `new HTMLImageElement()`]) test(`image node payload retained: ${source}`, t => {
  const entry = app(t, { 'index.tsx': source })
  assert.ok(nativeNodes(entry).includes('node-images'))
})
for (const source of [`<input />`, `<textarea />`, `<select />`, `<div contentEditable />`, `document.createElement('input')`, `new VirtualKeyboard()`, `document.createElement('INPUT')`, `new HTMLInputElement()`, `new HTMLTextAreaElement()`, `new HTMLSelectElement()`]) test(`native input payload retained: ${source}`, t => {
  const entry = app(t, { 'index.tsx': source })
  assert.ok(nativeNodes(entry).includes('node-inputs'))
})
for (const source of [`document.createElement(tag)`, `const create = document.createElement; create(tag)`, `element.innerHTML = markup`, `new DOMParser()`, `import { Something } from '@geastack/core'`, `import Widget from './missing'`, `__gea_Document.createElement(tag)`, `element['tagName'] = tag`, `document.write(markup)`, `range.createContextualFragment(markup)`, `const { createElement: make } = document; make(tag)`, `createElement(tag)`, `const { 'createElement': make } = document; make(tag)`]) test(`opaque native node creation keeps both payloads: ${source}`, t => {
  const entry = app(t, { 'index.tsx': source })
  assert.deepEqual(nativeNodes(entry), ['node-analysis-v1', 'node-images', 'node-inputs'])
})


test('a known bracket-access factory keeps ordinary nodes compact', t => {
  assert.deepEqual(nativeNodes(app(t, { 'index.tsx': `document['createElement']('button')` })), ['node-analysis-v1'])
})

for (const edge of ['top', 'right', 'bottom', 'left']) {
  for (const value of ['0', '-6px', 'auto', 'inherit', 'initial', '3px !important']) test(`fixed ${edge} ${value} omits other edges and percentages`, t => {
    const result = cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${edge}: '${value}' }} />` }))
    assert.deepEqual(result.filter(f => f.startsWith('css-position-')), [`css-position-${edge}`])
  })
  for (const value of ["'25%'", "'calc(50% - 3px)'", "'var(--offset)'", 'position']) test(`dynamic ${edge} ${value} retains percent representation`, t => {
    const result = cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${edge}: ${value} }} />` }))
    assert.deepEqual(result.filter(f => f.startsWith('css-position-')), [`css-position-${edge}`, `css-position-${edge}-percent`])
  })
}
for (const property of ['inset', 'insetInline', 'insetBlockStart']) test(`${property} conservatively retains every edge`, t => {
  const result = cssFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${property}: value }} />` }))
  assert.equal(result.filter(f => f.startsWith('css-position-')).length, 8)
})
for (const tag of ['input', 'textarea', 'virtual-list']) test(`${tag} native defaults retain positions`, t => {
  const result = cssFeatures(app(t, { 'index.tsx': `const n = <${tag} />` }))
  assert.equal(result.filter(f => f.startsWith('css-position-')).length, 8)
})

const classCapacity = entry => analyzeAllFeatures(entry).features.filter(f => f.startsWith('node-class-capacity-'))
for (const [source, count] of [
  ['const n = <div class="one" />', 1],
  ['const n = <div class="one two" />', 2],
  ['const n = <div class="one two three" />', 3],
  ['const n = <div class="one two three four" />', undefined],
  ["const n = <div class={{ one: true, two: condition }} />", 2],
  ["const n = <div class={condition ? 'one two' : 'three'} />", 2],
  ["const n = <div class={'one' + ' ' + 'two'} />", 2],
  ["const n = <div class={'one' + '' + 'two'} />", 1],
  ["const n = <div class={`one ${condition ? 'two' : ''}`} />", 2],
  ["function row(slot: number) { return <div class={`one slot-${slot}`} /> }", 2],
  ["function row({ slot }: { slot: number }) { return <div class={`one slot-${slot}`} /> }", 2],
  ["const n = <div class={dynamicClass} />", undefined],
  ["const n = <div class={{ ...classes }} />", undefined],
  ["const n = <div {...props} />", undefined],
  ["const n = <div class={`one ${value as number}`} />", undefined],
  ["const name = value as 'single'; const n = <div class={name} />", undefined],
  ["function row(name: 'single') { return <div class={name} /> }", undefined],
  ["const n = <div class={`one ${value as 'single'}`} />", undefined],
  ["const n = <div class={`one ${names[index]}`} />", undefined],
  ["const n = <div class='one' />; element.classList.add('two')", undefined],
  ["const n = <div classList={classes} />", undefined],
  ["const n = <div class:one={flag} />", undefined],
  ["element.getAttributeNode('class').value = names", undefined],
  ["const attrs = element.attributes; attrs[0].value = names", undefined],
  ["const n = <div class='one' />; element[key] = value", undefined],
  ["const n = <div class='one' />; Object.assign(element, properties)", undefined],
  ["const { assign } = Object; assign(element, properties)", undefined],
  ["const n = <list class='one' />", undefined],
  ["document.createElement('list')", undefined],
  ["const n = <input class='one' />", undefined],
  ["import { Widget } from 'opaque'; const n = <div class='one' />", undefined],
]) test(`class inline capacity proof: ${source}`, t => {
  const actual = classCapacity(app(t, { 'index.tsx': source }))
  assert.deepEqual(actual, count === undefined ? [] : [`node-class-capacity-v1-${count}`])
  const storage = analyzeAllFeatures(app(t, { 'index.tsx': source })).features.filter(f => f.startsWith('node-class-storage-'))
  assert.deepEqual(storage, count === undefined ? [] : [`node-class-storage-v1-${count}`])
})

test('class numeric property proof follows discovered relative stores', t => {
  const entry = app(t, {
    'index.tsx': "import { store } from './store'; const n = <div class={`status lit-${store.selected}`} />",
    'store.ts': 'class Store { selected = 0 }; export const store = new Store()',
  })
  assert.deepEqual(classCapacity(entry), ['node-class-capacity-v1-2'])
})

for (const separator of ['\t', '\n', '\r', '\v', '\f', '\x1f', ' ']) test(`class token proof retains native separator ${JSON.stringify(separator)}`, t => {
  const entry = app(t, { 'index.tsx': `const n = <div class={'one' + ${JSON.stringify(separator)} + 'two'} />` })
  assert.deepEqual(classCapacity(entry), ['node-class-capacity-v1-2'])
})

for (const source of [
  `const n = <div style={{ animation: 'fade 1s' }} />`,
  `el.style.animationName = name`, `el.style.WebkitAnimationDuration = time`,
  `el.style.transition = duration`, `el.style.setProperty('animation', value)`,
  `const n = <div data-anim="width" />`,
  `el.setAttribute('data-anim', value)`, `el.dataset.anim = 'width'`,
  `const set = el.setAttribute; set('data-anim', value)`,
  `const { 'setAttribute': set } = el; set('data-anim', value)`,
  `el.animate(frames, options)`, `el['animate'](frames, options)`,
  `const run = el.animate; run(frames)`, `const { 'animate': run } = el; run(frames)`,
  `new KeyframeEffect(el, frames)`, `new Animation(effect)`,
  `const run = Reflect.get(el, 'animate'); run(frames)`,
  `const run = Object.getOwnPropertyDescriptor(el, 'animate').value; run(frames)`,
  `const { [method]: run } = el; run(frames)`,
  `const add = sheet.insertRule; add(rule)`,
  `const { insertRule: add } = sheet; add(rule)`,
  `const run = el[method]; const alias = run; alias(frames)`,
  `const run = el[method]; run.call(el, frames)`,
  `el[method](frames)`, `el.style.cssText = styles`,
  `import './missing.css'`, `const n = <div {...props} />`,
]) test(`animation reachability survives dynamic and aliased use: ${source}`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': source })).includes('css-animations'))
})
for (const css of [
  '@keyframes move { to { width: 42px } }',
  '@-webkit-keyframes move { to { width: 42px } }',
  '.a { -webkit-animation: move 2s }',
  '.a { animation-name: var(--motion) }',
  String.raw`.a { anim\61tion: move 1s }`,
]) test(`CSS animation detection: ${css}`, t => {
  assert.ok(cssFeatures(app(t, { 'index.tsx': `import './app.css'`, 'app.css': css })).includes('css-animations'))
})
test('animation frame callbacks and ordinary CSS do not retain CSS animation storage', t => {
  const features = cssFeatures(app(t, { 'index.tsx': `import './app.css'; requestAnimationFrame(tick); cancelAnimationFrame(id); const n = <div />`, 'app.css': '.a { color: #123; width: 20px }' }))
  assert.ok(features.includes('css-analysis-v19'))
  assert.ok(!features.includes('css-animations'))
})

test('authored animation store methods do not enable native animation storage', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': "import agent from './store.js'; setInterval(() => agent.animate(), 50)",
    'store.js': "import { Store } from '@geastack/core'; class AgentStore extends Store { position = 0; animate() { this.position += 1 } }; const agent = new AgentStore(); export default agent",
  })).features
  for (const feature of ['css-animations', 'renderer-transforms', 'renderer-linear-gradients', 'renderer-radial-gradients']) assert.ok(!features.includes(feature), feature)
})

test('a boolean animate flag does not enable unrelated renderer caches', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': 'const animate = !dragging; if (animate) { paint() }',
  })).features
  for (const feature of ['css-animations', 'renderer-transforms', 'renderer-linear-gradients', 'renderer-radial-gradients']) assert.ok(!features.includes(feature), feature)
})

test('an opaque animate value still retains native animation storage', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': 'const animate = external.animate; animate()',
  })).features
  assert.ok(features.includes('css-animations'))
})

test('a local recursive animate frame callback does not enable unrelated renderer caches', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': 'function animate() { requestAnimationFrame(animate) }; requestAnimationFrame(animate)',
  })).features
  for (const feature of ['css-animations', 'renderer-transforms', 'renderer-linear-gradients', 'renderer-radial-gradients']) assert.ok(!features.includes(feature), feature)
})

test('default-exported animation store instances do not enable unrelated renderer caches', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': "import carousel from './carousel.js'; carousel.animate(16)",
    'carousel.js': "import { Store } from '@geastack/core'; class Carousel extends Store { offset = 0; animate(elapsed) { this.offset += elapsed } }; export default new Carousel()",
  })).features
  for (const feature of ['css-animations', 'renderer-transforms', 'renderer-linear-gradients', 'renderer-radial-gradients']) assert.ok(!features.includes(feature), feature)
})

test('a local animate function still retains styles used in its body', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': "function animate() { el.style.transform = 'rotate(10deg)' }; requestAnimationFrame(animate)",
  })).features
  assert.ok(features.includes('renderer-transforms'))
})

test('a reassigned animate function remains conservative', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': 'function animate() {}; animate = externalAnimation; requestAnimationFrame(animate)',
  })).features
  assert.ok(features.includes('css-animations'))
})

test('authored animation method bodies still retain actual native style operations', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': "class Agent { animate() { el.style.transform = 'rotate(10deg)' } }; const agent = new Agent(); agent.animate()",
  })).features
  assert.ok(features.includes('renderer-transforms'))
})

test('an opaque receiver cast to an authored animation class stays conservative', t => {
  const features = analyzeAllFeatures(app(t, {
    'index.tsx': "class Agent { animate() {} }; const agent = el as Agent; agent.animate()",
  })).features
  assert.ok(features.includes('css-animations'))
})


const alphaFeatures = entry => analyzeAllFeatures(entry).features.filter(feature => ['css-text-alpha', 'css-border-alpha'].includes(feature))
for (const color of ['#123', '#123f', '#112233', '#112233ff', 'inherit', 'currentColor']) test(`opaque text/border ${color} removes alpha storage`, t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx': `import './app.css'`, 'app.css': `.n { color: ${color}; border: 1px solid ${color} }` })), [])
})
for (const color of ['transparent', '#1230', '#11223388', 'rgba(1,2,3,.5)', 'rgb(1 2 3 / .5)', 'var(--missing)']) test(`nonopaque text/border ${color} retains alpha storage`, t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx': `import './app.css'`, 'app.css': `.n { color: ${color}; border-color: ${color} }` })), ['css-border-alpha', 'css-text-alpha'])
})
test('opaque custom colors prove across all definitions and inherited currentColor', t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx': `import './app.css'`, 'app.css': `:root { --ink:#123; --edge:var(--ink) } .a { --ink:#456; color:var(--ink); border-color:var(--edge); } .b { border: 1px solid; color:var(--absent,#abc) }` })), [])
})
for (const extra of ['.b {--ink:transparent}', '.b {--ink:var(--missing)}', '.b {--ink:var(--ink)}']) test(`uncertain custom color retains alpha: ${extra}`, t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx': `import './app.css'`, 'app.css': `:root {--ink:#123} .a { color:var(--ink); border-color:currentColor } ${extra}` })), ['css-border-alpha', 'css-text-alpha'])
})
test('text and border alpha proofs are independent', t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx': `const n = <div style={{ color:'#123', borderColor:'transparent' }} />` })), ['css-border-alpha'])
})
test('local immutable instance and literal palette getters prove dynamic colors', t => {
  assert.deepEqual(alphaFeatures(app(t, {
    'index.tsx': `import { store } from './store'; const n = <div style={{ color:store.ink, borderColor:store.edge }} />`,
    'store.ts': `import { palette as colors } from './colors'; class Store { index=0; ink='#123'; change(on:boolean) {this.ink=on?'#456':'#789'} get edge(){return colors[this.index]} } export const store=new Store()`,
    'colors.ts': `export const palette=['#123','#456','#789']`,
  })), [])
})
for (const extra of [
  `store.ink = network`,
  `const alias = store; alias.ink = 'transparent'`,
  `store[field] = 'transparent'`,
  `Object.assign(store, theme)`,
  `Reflect.set(store, 'ink', 'transparent')`,
]) test(`dynamic field write retains text alpha: ${extra}`, t => {
  assert.ok(alphaFeatures(app(t, { 'index.tsx': `class Store { ink='#123' } const store=new Store(); ${extra}; const n=<div style={{color:store.ink}} />` })).includes('css-text-alpha'))
})
for (const extra of [
  `palette[0]='transparent'`, `palette.push('transparent')`,
  `const alias=palette; alias[0]='transparent'`, `mutate(palette)`,
]) test(`escaped or mutated palette retains text alpha: ${extra}`, t => {
  assert.ok(alphaFeatures(app(t, { 'index.tsx': `const palette=['#123','#456']; let index=0; ${extra}; const n=<div style={{color:palette[index]}} />` })).includes('css-text-alpha'))
})
test('renamed imported palette mutations invalidate the original palette proof', t => {
  assert.ok(alphaFeatures(app(t, {
    'index.tsx': `import {palette} from './colors'; import './mutation'; let index=0; const n=<div style={{color:palette[index]}} />`,
    'colors.ts': `export const palette=['#123','#456']`,
    'mutation.ts': `import {palette as alias} from './colors'; alias[0]='transparent'`,
  })).includes('css-text-alpha'))
})
for (const source of [
  `class Store { ink='#123' } let store=new Store(); store=foreign; const n=<div style={{color:store.ink}} />`,
  `class Store { ink='#123' } const other:any=foreign; const n=<div style={{color:other.ink}} />`,
  `const ink='#123'; function render(ink:string) { return <div style={{color:ink}} /> }`,
  `const ink='#123'; function render() { function ink(){return network} return <div style={{color:ink}} /> }`,
  `const ink=network as '#123'; const n=<div style={{color:ink}} />`,
  `class Store { ink='#123'; constructor(){return foreign} } const store=new Store(); const n=<div style={{color:store.ink}} />`,
  `class Store { ink='#123' } function render(store:Store) {return <div style={{color:store.ink}} />}`,
]) test(`opaque receiver or binding cannot borrow a literal proof: ${source}`, t => {
  assert.ok(alphaFeatures(app(t, { 'index.tsx':source })).includes('css-text-alpha'))
})
for (const source of [
  `node.style.all='initial'`, `node.style.colorAlpha=value; node.style.borderAlpha=value`,
  `node.animate(keyframes)`, `import widget from 'external-widget'; const n=<widget />`,
]) test(`opaque style entry retains both alpha fields: ${source}`, t => {
  assert.deepEqual(alphaFeatures(app(t, { 'index.tsx':source })), ['css-border-alpha', 'css-text-alpha'])
})

test('recursive palettes retain alpha instead of recursing in the proof', t => {
  assert.ok(alphaFeatures(app(t, {'index.tsx': `const palette=[palette[0]]; const n=<div style={{color:palette[0]}} />`})).includes('css-text-alpha'))
})

for (const escape of [
  'foreign(store)', 'foreign({store})', 'window.shared=store',
  'function expose(){return store}', 'unknown.call(store)',
]) test(`escaping local receivers retain dynamic color alpha: ${escape}`, t => {
  assert.ok(alphaFeatures(app(t, {'index.tsx': `class Store {ink='#123'} const store=new Store(); ${escape}; const n=<div style={{color:store.ink}} />`})).includes('css-text-alpha'))
})
test('constructor callback receiving this cannot establish an opaque-color proof', t => {
  assert.ok(alphaFeatures(app(t, {'index.tsx': `class Store {ink='#123'; constructor(mutate:any){mutate(this)} } const store=new Store(foreign); const n=<div style={{color:store.ink}} />`})).includes('css-text-alpha'))
})

for (const definition of [
  `class Store {ink='#123'; mutate=foreign} const store=new Store(); store.mutate()`,
  `class Store {ink='#123'; mutate(){} } const store=new Store(); store.mutate=foreign; store.mutate()`,
  `const store={ink:'#123', mutate:foreign}; store.mutate()`,
]) test(`opaque methods cannot mutate a supposedly proven receiver: ${definition}`, t => {
  assert.ok(alphaFeatures(app(t, {'index.tsx': `${definition}; const n=<div style={{color:store.ink}} />`})).includes('css-text-alpha'))
})

for (const escape of [
  'const wrapper=[store]; foreign(wrapper)',
  'const wrapper={store}; foreign(wrapper.store)',
  'const wrapper={nested:[store]}; foreign(wrapper.nested)',
  'let alias=store; foreign(alias)',
]) test(`nested receiver escape retains alpha: ${escape}`, t => {
  assert.ok(alphaFeatures(app(t, {'index.tsx': `class Store {ink='#123'} const store=new Store(); ${escape}; const n=<div style={{color:store.ink}} />`})).includes('css-text-alpha'))
})

for (const selector of ['.x::before', '.x::after', '.x:before', '.x:after', '.x::BEFORE']) {
  test(`generated pseudo-elements retained for ${selector}`, (t) => {
    const entry = app(t, { 'index.tsx': "import './base.css'", 'base.css': "@import './nested.css';", 'nested.css': `@media (min-width: 1px) { ${selector} { color: red; } }` })
    assert.ok(analyzeAllFeatures(entry).features.includes('css-pseudo-elements'))
  })
}
for (const source of [
  `sheet.insertRule('.x::after { content: "x"; }')`,
  `element.style.content = text`,
  `const node = <div style={{ content: 'x' }} />`,
  `element.style.cssText = remoteCss`,
]) test(`generated pseudo-elements retained by ${source}`, (t) => {
  assert.ok(analyzeAllFeatures(app(t, { 'index.tsx': source })).features.includes('css-pseudo-elements'))
})
for (const selector of ['.x:first-child', '.x:last-child', '.x::first-line', '.x:hover', '.before .after']) {
  test(`ordinary selector does not enable generated pseudo-elements: ${selector}`, (t) => {
    const features = analyzeAllFeatures(app(t, { 'index.tsx': "import './style.css'", 'style.css': `${selector} { color: red; justify-content: center; }` })).features
    assert.ok(!features.includes('css-pseudo-elements'))
    if (selector.includes('first-line')) assert.ok(features.includes('css-first-line'))
  })
}

// The original Bouncing Balls JSX uses CSS circles, without a canvas arc call.
for (const declaration of [
  'border-radius: 50%', 'border-radius: 16px',
  ...['top-left', 'top-right', 'bottom-left', 'bottom-right'].map(corner => `border-${corner}-radius: 8px`),
  'border-radius: var(--radius)',
]) test(`rounded CSS keeps the span cache: ${declaration}`, t => {
  const entry = app(t, { 'index.tsx': "import './balls.css'; const ball = <div class='ball' />", 'balls.css': `.ball { ${declaration} }` })
  assert.ok(analyzeSourceHostBindings(entry).features.includes('renderer-circles'))
})
for (const value of ['0', '0px', '0 0', '0px 0 0px 0']) test(`zero radius does not allocate circle caches: ${value}`, t => {
  const entry = app(t, { 'index.tsx': "import './balls.css'", 'balls.css': `.ball { border-radius: ${value} }` })
  assert.ok(!analyzeSourceHostBindings(entry).features.includes('renderer-circles'))
})
for (const source of [
  'const ball = <div style={{ borderRadius: radius }} />',
  'ball.style.borderTopLeftRadius = radius',
  "ball.style.setProperty('border-radius', radius)",
]) test(`dynamic radius retains the span cache: ${source}`, t => {
  assert.ok(analyzeSourceHostBindings(app(t, {'index.tsx': source})).features.includes('renderer-circles'))
})
test('unrounded video canvas still strips shape caches', t => {
  assert.deepEqual(analyzeSourceHostBindings(app(t, {'index.tsx': 'const frame = <canvas />; ctx.drawImage(image, 0, 0)'})).features, ['renderer-analysis-v1'])
})

for (const binding of ['Display', 'Display as Screen']) test(`Display import ${binding} does not inject native nodes or unknown CSS bounds`, t => {
  const local = binding === 'Display' ? 'Display' : 'Screen'
  const entry = app(t, { 'index.tsx': `import { ${binding}, mount } from '@geastack/core';
    ${local}.setFrameRate(120); ${local}.setFlushConfig({ rows: 64, depth: 2 });
    const node = <div style={{ padding: '8px', borderRadius: '6px', fontSize: '12px' }}>FPS</div>` })
  assert.deepEqual(nativeNodes(entry), ['node-analysis-v1'])
  const features = rangeFeatures(entry)
  assert.ok(!features.includes('css-ranges-unknown'))
  assert.ok(features.includes('css-range-padding-px-8'))
  assert.ok(features.includes('css-range-radius-px-6'))
})
for (const mutation of [
  `Screen.setDevicePixelRatio(8)`,
  `Screen['setDevicePixelRatio'](8)`,
  `const { setDevicePixelRatio: changeScale } = Screen; changeScale(8)`,
  `Screen[method](8)`,
]) test(`Display runtime scaling keeps range bounds conservative: ${mutation}`, t => {
  const entry = app(t, { 'index.tsx': `import { Display as Screen } from '@geastack/core'; ${mutation}; const node = <div style={{ padding: '8px' }} />` })
  assert.ok(rangeFeatures(entry).includes('css-ranges-unknown'))
})

function commonStyleFeatures(entry) {
  return analyzeAllFeatures(entry).features.filter(feature => baseStyleFamilies.includes(feature)).sort()
}
for (const [property, value, families] of [
  ['flexDirection', 'column', ['flex-direction']], ['flexFlow', 'column wrap', ['flex-direction']],
  ['justifyContent', 'center', ['justify-content']], ['placeContent', 'center', ['justify-content']],
  ['alignItems', 'center', ['align-items']], ['placeItems', 'center', ['align-items']],
  ['boxSizing', 'border-box', ['box-sizing']], ['activeBackgroundColor', '#f00', ['active-background']],
  ['activeBackground', '#f00', ['active-background']], ['minHeight', '20px', ['min-height']],
  ['minInlineSize', '20px', ['min-height']], ['maxWidth', '20px', ['max-width']],
  ['maxBlockSize', '20px', ['max-width']], ['lineHeight', '1.5', ['line-height-multiplier']],
  ['font', '12px/1.5 Inter', ['line-height-multiplier']], ['width', 'calc(100% - 8px)', ['width-expressions']],
  ['inlineSize', '20px', ['width-expressions']], ['margin', '0 auto', ['margin-auto']],
  ['marginInlineStart', 'var(--edge)', ['margin-auto']],
]) test(`v16 retains common field authored through ${property}`, t => {
  assert.deepEqual(commonStyleFeatures(app(t, { 'index.tsx': `const n = <div style={{ ${property}: ${JSON.stringify(value)} }} />` })), families.map(f => `css-${f}`).sort())
})
for (const value of ['20', '20px', '50%', 'auto', 'initial', 'inherit', 'unset'])
  test(`v16 simple width needs no deferred storage: ${value}`, t => {
    assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': `const n = <div style={{ width: '${value}' }} />`})), [])
  })
for (const value of ['max-content', 'min-content', 'fit-content', '2em', '30vw', 'var(--size)'])
  test(`v16 deferred width retains storage: ${value}`, t => {
    assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': `const n = <div style={{ width: '${value}' }} />`})), ['css-width-expressions'])
  })
for (const source of [
  'element.style[name] = value', 'mutate(element.style)', 'import Unknown from "opaque-package"; Unknown()',
  'const n = <div style={{...unknown}} />', 'const n = <input />',
  'import { VirtualList } from "@geastack/core"; const n = new VirtualList()',
  'const f = document.createElement; f(tag)',
]) test(`v16 unknown or native defaults retain common fields: ${source}`, t => {
  assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': source})), [...baseStyleFamilies].sort())
})
test('v16 dynamic values keep only their known common family', t => {
  assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': 'element.style.width = liveWidth; element.style.marginLeft = edge'})), ['css-margin-auto', 'css-width-expressions'])
})
test('v16 imported CSS and shorthands participate in the common field proof', t => {
  assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': "import './a.css'", 'a.css': "@import './b.css'; .x { margin: 0 2px; font-size: 12px; }", 'b.css': '.x { place-items: center; active-background: red; }'})), ['css-active-background', 'css-align-items'])
})
test('v16 Display import and fixed numeric margins preserve common-field elimination', t => {
  assert.deepEqual(commonStyleFeatures(app(t, {'index.tsx': 'import { Display } from "@geastack/core"; Display.setFrameRate(120); const n = <div style={{margin:"0 2px", fontSize:12}} />'})), [])
})

function defaultStyleFeatures(entry) {
  return analyzeAllFeatures(entry).features.filter(feature => defaultStyleFamilies.includes(feature)).sort()
}
for (const [property, value, families] of [
  ['margin','-2px 3px',['margins']], ['marginInlineStart','var(--edge)',['margins']],
  ['padding','1px 2px',['padding']], ['paddingBlockEnd','calc(2px + 3%)',['padding']],
  ['flex','1 0 20px',['flex-factors']], ['flexGrow',2,['flex-factors']], ['flexShrink',0,['flex-factors']],
  ['gap','2px',['gap']], ['rowGap','3%',['gap']], ['gridColumnGap','4px',['gap']],
  ['border','1px solid red',['border-colors','border-widths']], ['borderInline','solid',['border-colors','border-widths']],
  ['borderStyle','solid',['border-colors','border-widths']], ['borderWidth','2px',['border-widths']],
  ['borderInlineStartWidth','1px',['border-widths']], ['borderColor','currentColor',['border-colors']],
  ['borderBlockEndColor','var(--ink)',['border-colors']], ['fontWeight','bold',['font-weight']],
  ['font','italic bold 12px Inter',['font-weight']], ['textAlign','center',['text-align']],
  ['whiteSpace','pre-wrap',['white-space']], ['textWrap','nowrap',['white-space']],
  ['textOverflow','ellipsis',['text-overflow']],
]) test(`v17 retains authored default field: ${property}`, t => {
  const source = `const n = <div style={{ ${property}: ${JSON.stringify(value)} }} />`
  assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':source})), families.map(f => `css-${f}`).sort())
})
for (const source of [
  'element.style[name] = value', 'mutate(element.style)', 'import Unknown from "opaque-package"; Unknown()',
  'const n = <div style={{...unknown}} />', 'const n = <input />',
  'import { VirtualList } from "@geastack/core"; const n = new VirtualList()',
  'const f = document.createElement; f(tag)',
]) test(`v17 opaque/native defaults retain all default fields: ${source}`, t => {
  assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':source})), [...defaultStyleFamilies].sort())
})
test('v17 dynamic known fields retain only their own families', t => {
  assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':'element.style.padding = edge; element.style.fontWeight = weight'})), ['css-font-weight','css-padding'])
})
test('v17 imported CSS, variables and shorthand inheritance preserve storage', t => {
  assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':"import './a.css'",'a.css':"@import './b.css'; .x { --edge: 3px; margin: var(--edge); }",'b.css':'.x { border: inherit; text-align: unset; }'})), ['css-border-colors','css-border-widths','css-margins','css-text-align'])
})
for (const [tag, features] of [['strong',['css-font-weight']],['pre',['css-white-space']],['th',['css-font-weight','css-text-align']]])
  test(`v17 intrinsic ${tag} defaults remain available`, t => {
    assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':`const n = <${tag}>hello</${tag}>`})), features)
  })
test('v17 circle radius, color, font size and Display do not require default fields', t => {
  assert.deepEqual(defaultStyleFeatures(app(t, {'index.tsx':'import { Display } from "@geastack/core"; Display.setFrameRate(120); const n = <span style={{borderRadius:8,color:"red",fontSize:16}}>text</span>'})), [])
})


function hasCustomProperties(t, source, files = {}) {
  return analyzeAllFeatures(app(t, { 'index.tsx': source, ...files })).features.includes('css-custom-properties')
}
for (const source of [
  'const n = <div style={{ "--color": "red" }} />',
  'element.style.setProperty("--offset", offset)',
  'element.style.setProperty(dynamicName, value)',
  'const n = <div style={unknownStyle} />',
  'const n = <div style={{ color: "var(--tone, red)" }} />',
  'element.style.color = "var(--tone)"',
  'import { Card } from "opaque-component"; const n = <Card />',
  'const n = <input />',
]) test(`custom-property storage retained for ${source}`, t => assert.ok(hasCustomProperties(t, source)))
test('custom-property definitions in imported CSS retain both stores and dependency tracking', t => {
  assert.ok(hasCustomProperties(t, "import './a.css'", {
    'a.css': "@import './b.css'; .x { color: var(--tone); }",
    'b.css': '.x { --tone: red; }',
  }))
})
for (const source of [
  'const n = <div style={{ left: ball.x, top: ball.y, backgroundColor: ball.color }} />',
  'element.style.left = x; element.style.top = y',
  'const n = <span style={{ color: "red", fontSize: 16 }}>FPS</span>',
]) test(`unused custom-property storage removed for ${source}`, t => assert.equal(hasCustomProperties(t, source), false))


function circleBounds(t, source, files = {}) {
  return analyzeAllFeatures(app(t, { 'index.tsx': source, ...files })).features
}
test('CSS-only circle bounds carry an independent whole-source proof', t => {
  const features = circleBounds(t, "import './shape.css'; const n=<div style={{left:x,top:y}} />", {'shape.css':'.ball { border-radius: 8; }'})
  assert.ok(features.includes('css-circle-cache-v1'))
  assert.ok(features.includes('css-range-radius-raw-8'))
  assert.ok(features.includes('css-range-radius-px-0'))
  assert.ok(!features.includes('css-circle-cache-unbounded'))
})
for (const source of [
  'ctx.arc(0,0,8,0,6.28)', 'ctx.fillCircle(0,0,8)', 'ctx.fillCircleRgb565(0,0,r,color)', 'ctx.drawImageCircle(image,0,0,w,h)', 'ctx.fillRoundedRect(0,0,16,16,8)',
  'const f=ctx.fillCircle; f(0,0,r)', 'const {arc: draw}=ctx; draw(0,0,r,0,6)',
  'ctx["fill"+"Circle"](0,0,r)', 'ctx[name](r)',
  'const n=<div style={{borderRadius:r}} />', 'const n=<div style={{borderRadius:"50%"}} />',
  'const n=<div style={unknownStyle} />', 'import {Circle} from "opaque"; const n=<Circle />',
  'const n=<button />', 'const n=<canvas />', 'const n=<input />',
  'Display.setDevicePixelRatio(ratio)',
]) test(`circle caches keep full bounds for ${source}`, t => {
  const features = circleBounds(t, source)
  assert.ok(features.includes('css-circle-cache-v1'))
  assert.ok(features.includes('css-circle-cache-unbounded'), source)
})


for (const source of [
  'ctx.fillTrianglesRgb565Sorted(xs,ys,colors,order,count)',
  'ctx.fillTriangleRgb565(x0,y0,x1,y1,x2,y2,color)',
  'const draw=ctx.fillTrianglesRgb565Sorted; draw(xs,ys,colors,order,count)',
  'const {fillTrianglesRgb565Sorted:draw}=ctx; draw(xs,ys,colors,order,count)',
  'const { ["fill"+"TrianglesRgb565Sorted"]:draw }=ctx; draw(xs,ys,colors,order,count)',
  'const { [key]:draw }=ctx; draw(args)',
  'ctx["fill"+"TrianglesRgb565Sorted"](xs,ys,colors,order,count)',
  'ctx[name](args)', 'Reflect.get(ctx,name)(args)',
  'Object.getOwnPropertyDescriptor(ctx,name).value(args)',
  'eval(source)', 'new Function(source)()',
  'import {drawScene} from "opaque-native"; drawScene()',
  'import {Canvas} from "@geastack/core"; const ctx=new Canvas()',
  '__gea_Native.drawScene()',
]) test(`triangle occlusion proof retains reachable or opaque drawing: ${source}`, t => {
  const features=analyzeAllFeatures(app(t,{'index.tsx':source})).features
  assert.ok(features.includes('renderer-occlusion-v1'))
  assert.ok(features.includes('renderer-occlusion-triangles'))
})
for (const source of [
  'const n=<div style={{borderRadius:8,left:x,top:y}} />',
  'import {Display} from "@geastack/core"; Display.setFrameRate(120); const n=<span>FPS</span>',
  'ctx.fillCircle(20,20,8)',
]) test(`triangle occlusion proof removes unused scratch: ${source}`, t => {
  const features=analyzeAllFeatures(app(t,{'index.tsx':source})).features
  assert.ok(features.includes('renderer-occlusion-v1'))
  assert.ok(!features.includes('renderer-occlusion-triangles'))
})


const sizePercentFamilies = ['css-height-percent', 'css-width-percent']
const sizePercentUsage = entry => analyzeAllFeatures(entry).features.filter(feature => sizePercentFamilies.includes(feature))
for (const value of ['0', '16', '-2px', '.5em', '2rem', '100vw', '100vh', '3vmin', '5dvh', '2lh', '1cm', 'auto', 'inherit', 'min-content', 'max-content', 'fit-content', '12px !important']) {
  test(`v19 non-percentage dimension ${value} removes percentage storage`, t => {
    const entry = app(t, {'index.tsx': `import './style.css'`, 'style.css': `.x {width:${value};height:${value};}`})
    assert.deepEqual(sizePercentUsage(entry), [])
    assert.ok(analyzeAllFeatures(entry).features.includes('css-analysis-v19'))
  })
}
for (const value of ['50%', '0%', 'calc(100% - 2px)', 'min(50%, 20px)', 'var(--size)', 'var(--size, 10px)', 'garbage', '10px 20px']) {
  test(`v19 possible percentage ${value} retains only its dimension`, t => {
    const entry = app(t, {'index.tsx': `import './style.css'`, 'style.css': `.x {width:${value};height:20px;}`})
    assert.deepEqual(sizePercentUsage(entry), ['css-width-percent'])
  })
}
for (const source of [
  'const x = <div style={{width: external, height: other}} />',
  'node.style.width = value; node.style.height = value',
  'const x = <div style={opaque} />',
  'node.style.setProperty(name, value)',
  'const x = <div style={{inlineSize: value}} />',
  `import './style.css'`,
  `import {Input} from '@geastack/core'; const x = <Input/>`,
  `import {Card} from 'opaque-package'; const x = <Card/>`,
]) {
  test(`v19 opaque dimension sources retain both fields: ${source}`, t => {
    const entry = app(t, {'index.tsx':source,'style.css':'.x{block-size:50%;}'})
    assert.deepEqual(sizePercentUsage(entry), sizePercentFamilies)
  })
}
test('v19 fixed logical dimensions and class switches preserve the complete percentage union', t => {
  const entry=app(t, {'index.tsx':`import './style.css'; const x=<div class={condition?'fixed':'fraction'}/>`,'style.css':'.fixed{inline-size:40px;block-size:2rem}.fraction{height:25%}'})
  assert.deepEqual(sizePercentUsage(entry), ['css-height-percent'])
})

test('compact auxiliary proof removes only unreachable node owners', (t) => {
  const entry = app(t, {
    'index.tsx': `import { Component, Store, mount } from '@geastack/core';
      class Data extends Store { items: { x: number }[] = [{ x: 0 }]; tick() { for (let i = 0; i < this.items.length; i++) this.items[i].x += 1; } }
      const data = new Data(); const view = <div class="root">{data.items.map(item => <span style={{ left: item.x }}>FPS</span>)}</div>;`,
  })
  const features = analyzeAllFeatures(entry).features
  assert.ok(features.includes('node-aux-v1'))
  for (const feature of ['node-listeners', 'node-attributes', 'node-default-styles', 'css-position-left-percent']) assert.ok(!features.includes(feature), feature)
})

for (const [source, retained] of [
  ['const view = <div onmousedown={handler} />', ['node-listeners', 'node-attributes']],
  ['const view = <div click={handler} />', ['node-listeners', 'node-attributes']],
  ['const view = <div id="counter" />', ['node-attributes']],
  ['const view = <h1>Title</h1>', ['node-listeners', 'node-attributes', 'node-default-styles']],
  ['node[operation](value)', ['node-listeners', 'node-attributes', 'node-default-styles']],
  ['const view = <div {...props} />', ['node-listeners', 'node-attributes', 'node-default-styles']],
  ['import widget from "opaque-package"; widget()', ['node-listeners', 'node-attributes', 'node-default-styles']],
]) test(`compact auxiliary proof retains opaque/used storage: ${source}`, (t) => {
  const features = analyzeAllFeatures(app(t, { 'index.tsx': source })).features
  for (const feature of retained) assert.ok(features.includes(feature), feature)
})

test('compact numeric position proof distinguishes numbers, percentages and opaque values', (t) => {
  const entry = app(t, {
    'index.tsx': `import './style.css'; const state = { x: 12 }; const view = <div style={{ left: state.x }} />;`,
    'style.css': '.badge { top: 3vmin; left: 2em; }',
  })
  const features = analyzeAllFeatures(entry).features
  assert.ok(!features.includes('css-position-left-percent'))
  assert.ok(!features.includes('css-position-top-percent'))
})
for (const value of ['"12%"', 'external', 'external as number', '(external as any).x', 'Math.random() ? 5 : "10%"']) {
  test(`compact numeric position proof retains percentage storage: ${value}`, (t) => {
    const features = analyzeAllFeatures(app(t, { 'index.tsx': `const view = <div style={{ left: ${value} }} />` })).features
    assert.ok(features.includes('css-position-left-percent'))
  })
}


test('compact numeric slots remain numbers when unrelated objects escape', t => {
  const features = analyzeAllFeatures(app(t, {'index.tsx': `import { Display } from '@geastack/core';
    class Ball { x: int = 0; y: int = 0; }
    const balls: Ball[] = []; balls.push({x:0,y:0}); Display.setFlushConfig({rows:64,depth:2});
    const view=<div>{balls.map(ball=><div style={{left:ball.x,top:ball.y}} />)}</div>;`})).features
  for (const feature of ['css-position-left-percent','css-position-top-percent','node-listeners','node-attributes','node-default-styles']) assert.ok(!features.includes(feature), feature)
})

for (const [source, css, expected] of [
  ['const n=<div style={{color:"red"}}/>', '', []],
  ['const n=<div style={{display:"none"}}/>', '', ['css-display-explicit']],
  ['const n=<div style={{lineHeight:18}}/>', '', ['css-line-height']],
  ['node.style.font="16px/2 sans-serif"', '', ['css-line-height']],
  ["import './style.css'", '.a{line-height:normal;display:inherit}', ['css-display-explicit','css-line-height']],
  ['const n=<div style={opaque}/>', '', ['css-display-explicit','css-line-height']],
  ['const n=<input/>', '', ['css-display-explicit','css-line-height']],
]) test(`compact default storage follows all source uses: ${source}`, t => {
  const features=analyzeAllFeatures(app(t, {'index.tsx':source,'style.css':css})).features
  assert.ok(features.includes('css-storage-v1'))
  assert.deepEqual(features.filter(f=>['css-line-height','css-display-explicit'].includes(f)),expected)
})


test('implicit root selectors and authored DOM lookups retain attribute identity', t => {
  for (const source of ['document.getElementById("app")','document.querySelector(selector)','Document.ensureAppRoot("custom")'])
    assert.ok(analyzeAllFeatures(app(t, {'index.tsx':source})).features.includes('node-attributes'))
  for (const selector of ['#app','[data-state="active"]',String.raw`\23 app`])
    assert.ok(analyzeAllFeatures(app(t, {'index.tsx':"import './style.css'",'style.css':`${selector}{color:red}`})).features.includes('node-attributes'),selector)
})


test('CSS colors and quoted declaration contents do not allocate attribute owners', t => {
  const features=analyzeAllFeatures(app(t, {'index.tsx':"import './style.css'",'style.css':'.app{color:#fff;background:#000} @media (monochrome){.ball{color:#000}}'})).features
  assert.ok(!features.includes('node-attributes'))
})
test('attribute selectors retain owners even when quoted values contain delimiters', t => {
  const features=analyzeAllFeatures(app(t, {'index.tsx':"import './style.css'",'style.css':'[title=";{}"]{color:red}'})).features
  assert.ok(features.includes('node-attributes'))
})


test('UUID string replacement does not enable stylesheet caches', (t) => {
  const entry = app(t, { 'index.tsx': "const uuid = 'xxxx-yyyy'.replace(/[xy]/g, letter => Math.random().toString(16)); const view = <div />" })
  const features = analyzeSourceHostBindings(entry).features
  assert.ok(!features.includes('renderer-linear-gradients'))
  assert.ok(!features.includes('renderer-radial-gradients'))
  assert.ok(!features.includes('renderer-transforms'))
})

for (const source of [
  'function uuid(pattern: string) { return pattern.replace(/[xy]/g, letter => letter) }',
  'let pattern = "xxxx-yyyy"; pattern = "yyyy-xxxx"; pattern.replace(/[xy]/g, letter => letter)',
  'function uuid(pattern: "xxxx" | "yyyy") { return pattern["replace"](/[xy]/g, letter => letter) }',
  'class Generator { pattern: string = "xxxx"; uuid() { return this.pattern.replace(/x/g, () => "0") } }',
  'function pattern(): string { return "xxxx" } pattern().replace(/x/g, () => "0")',
]) test(`primitive string replacement does not enable stylesheet caches: ${source}`, t => {
  const features = analyzeAllFeatures(app(t, { 'index.tsx': source })).features
  for (const feature of ['renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms', 'css-transforms']) {
    assert.ok(!features.includes(feature), feature)
  }
})

test('imported string replacement does not enable stylesheet caches', t => {
  const entry = app(t, {
    'index.tsx': 'import { pattern } from "./uuid"; pattern.replace(/x/g, () => "0")',
    'uuid.ts': 'export function template(): string { return "xxxx" } export const pattern = template()',
  })
  const features = analyzeAllFeatures(entry).features
  assert.ok(!features.includes('renderer-transforms'))
  assert.ok(!features.includes('renderer-linear-gradients'))
  assert.ok(!features.includes('renderer-radial-gradients'))
})

for (const source of [
  'interface Sheet { replace(css: string): void } function update(sheet: Sheet, css: string) { sheet.replace(css) }',
  'interface Sheet { replace(css: string): void } function update(value: string | Sheet, css: string) { value.replace(css) }',
  'function update(value: any, css: string) { value.replace(css) }',
]) test(`non-string replacement retains stylesheet caches: ${source}`, t => {
  const features = analyzeAllFeatures(app(t, { 'index.tsx': source })).features
  for (const feature of ['renderer-linear-gradients', 'renderer-radial-gradients', 'renderer-transforms', 'css-transforms']) {
    assert.ok(features.includes(feature), feature)
  }
})

test('string replacement callbacks still contribute stylesheet operations', t => {
  const source = 'function uuid(pattern: string) { return pattern.replace(/x/g, () => { sheet.replace(cssFromNetwork); return "0" }) }'
  const features = analyzeAllFeatures(app(t, { 'index.tsx': source })).features
  assert.ok(features.includes('renderer-transforms'))
  assert.ok(features.includes('renderer-linear-gradients'))
  assert.ok(features.includes('renderer-radial-gradients'))
})

test('unknown stylesheet replacement retains stylesheet caches', (t) => {
  const entry = app(t, { 'index.tsx': 'sheet.replace(cssFromNetwork)' })
  const features = analyzeSourceHostBindings(entry).features
  assert.ok(features.includes('renderer-linear-gradients'))
  assert.ok(features.includes('renderer-radial-gradients'))
  assert.ok(features.includes('renderer-transforms'))
})


test('imported image and JSON data are not parsed as executable renderer code', (t) => {
  const entry = app(t, {
    'index.tsx': "import portrait from './portrait.jpg'; import config from './config.json'; const image = <img src={portrait} alt={config.name} />",
    'portrait.jpg': '\u00ff\u00d8 invalid TS image bytes',
    'config.json': '{"name": "Test"}'
  })
  assert.deepEqual(analyzeSourceHostBindings(entry).features, ['renderer-analysis-v1'])
})

test('JSON data used as an unknown style still retains renderer support', (t) => {
  const entry = app(t, {
    'index.tsx': "import config from './config.json'; const image = <div style={config} />",
    'config.json': '{"background": "linear-gradient(red, blue)"}'
  })
  assert.ok(analyzeSourceHostBindings(entry).features.includes('renderer-linear-gradients'))
})


test('canvas inference retains services for imports lowered to native asset loaders', (t) => {
  const entry = app(t, {
    'index.tsx': "import { Display } from '@geastack/core'; import image from './picture.png'; Display.ctx.drawImage(image, 0, 0)",
    'picture.png': 'image data',
  })
  assert.ok(!analyzeAllFeatures(entry).features.includes('runtime-canvas-only'))
})


test('streaming PCM playback independently enables the audio capability', (t) => {
  const entry = app(t, { 'index.tsx': 'const output = new PcmAudioStream(24000); output.writeBase64("AAA=")' })
  assert.ok(analyzeSourceHostBindings(entry).bindings.includes('audio'))
})
