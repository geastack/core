import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const testDir = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(testDir, '../../..')
const tempRoot = path.join(testDir, '.build', 'static-css-codegen')
const appDir = path.join(tempRoot, 'app')
const outDir = path.join(tempRoot, 'out')
const tapeOutDir = path.join(tempRoot, 'out-tape')

fs.mkdirSync(appDir, { recursive: true })
fs.writeFileSync(path.join(appDir, 'index.ts'), `
import './styles.css'

export const appName = 'static-css-codegen'
`)

fs.writeFileSync(path.join(appDir, 'styles.css'), `
.spin {
  rotate: 45deg;
  animation: turn 1s linear forwards;
}

.ordered-rotation { transform: rotate(90deg) rotate3d(1,0,0,60deg); }
.repeated-rotation { transform: rotate(45deg) rotate(45deg); }
.scale-depth { transform: scaleZ(2) scaleZ(3); }
.shaded-border { border: 10px groove #000000; border-left: 8px ridge #804020; }

.zoom:hover { background-color: #00ff00; }

.axis-turn { rotate: 180deg 1 1 0; scale: -100% 200% 150%; }

.zoom {
  scale: 1.25;
  --ink: #112233;
  --gap: 8px;
  --panel-w: 72%;
  color: var(--ink);
  background-color: var(--paper, #abcdef);
  border: 0;
  border-left: 0;
  border-bottom: 1px solid rgba(255, 255, 255, 0.25);
  border-top: 2px solid var(--ink);
  filter: blur(2px);
  flex-shrink: 0;
  gap: var(--gap);
  width: var(--panel-w, 50%);
  animation: grow 1s linear forwards;
}

.scene {
  --base-1: #112233;
  --base-2: #445566;
  --base-3: #778899;
  background: linear-gradient(to bottom, transparent 60%, rgba(4, 13, 23, 0.32)),
    radial-gradient(120% 90% at 50% -22%, rgba(255, 211, 123, 0.52), transparent 54%),
    linear-gradient(158deg, var(--base-1) 0%, var(--base-2) 58%, var(--base-3) 135%);
  box-shadow: 0 9px 23px rgba(0, 0, 0, 0.22);
  grid-template-columns: minmax(0, 1fr) auto 19px;
  mask-image: linear-gradient(to right, #000 0, #000 calc(100% - 15px), transparent 100%);
}

.auto-box {
  width: auto;
  height: auto;
}

@keyframes turn {
  from { rotate: 45deg; }
  to { rotate: 90deg; }
}

@keyframes grow {
  from { scale: 1.25; }
  to { scale: 1.5; }
}

@keyframes ink {
  from { color: var(--ink); }
  to { color: var(--paper, #abcdef); }
}
`)

execFileSync(process.execPath, [
  path.join(repoRoot, 'packages/core/scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir,
  '--entry', 'index.ts',
  '--out-dir', outDir,
  '--gea-embedded-compat',
  '--gea-ir-backend',
  '--allow-any',
], {
  cwd: repoRoot,
  stdio: 'pipe',
  env: { ...process.env, GEA_STATIC_CSS_TAPE: '0' },
})

const generated = fs.readFileSync(path.join(outDir, 'gea-style-registration.cppfrag'), 'utf8')

assert.match(generated, /registerStaticRule\("shaded-border", "border", "10px groove #000000"/)
assert.match(generated, /registerStaticRule\("shaded-border", "border-left", "8px ridge #804020"/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"zoom"[^;]*Property::BorderRelief,\s*0/)
assert.match(generated, /registerStaticPropertyRule\([^;]*"zoom"[^;]*Property::BorderBottomRelief,\s*0/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"zoom"[^;]*Property::BorderColorCurrent,\s*1/)

assert.match(generated, /registerStaticRule\("ordered-rotation", "transform", "rotate\(90deg\) rotate3d\(1,0,0,60deg\)"/)
assert.match(generated, /registerStaticRule\("repeated-rotation", "transform", "rotate\(45deg\) rotate\(45deg\)"/)
assert.match(generated, /registerStaticTransformRule\([^;]*"scale-depth"[^;]*8192[^;]*1000[^;]*1000[^;]*nullptr, 6000/)
assert.match(generated, /"\.zoom:hover"[^;]*\{nullptr, nullptr, \{"zoom"\}, false, false, false, true\}/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*Property::RotateAngle,\s*450/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"axis-turn"[^;]*Property::RotateAngle,\s*1800[^;]*Property::RotateAxisX,\s*1000000[^;]*Property::RotateAxisY,\s*1000000/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"axis-turn"[^;]*Property::RotateAxisZ,\s*0/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"axis-turn"[^;]*Property::ScaleX,\s*-1000[^;]*Property::ScaleY,\s*2000[^;]*Property::ScaleZ,\s*1500/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*Property::ScaleX,\s*1250[^;]*Property::ScaleY,\s*1250/)
assert.match(generated, /registerStaticPropertyKeyframeRule\("turn",\s*0,\s*gea::embedded::ui::Property::RotateAngle,\s*450\)/)
assert.match(generated, /registerStaticPropertyKeyframeRule\("turn",\s*1000,\s*gea::embedded::ui::Property::RotateAngle,\s*900\)/)
assert.match(generated, /registerStaticPropertyKeyframeRule\("grow",\s*0,\s*gea::embedded::ui::Property::ScaleX,\s*1250\)/)
assert.match(generated, /registerStaticPropertyKeyframeRule\("grow",\s*0,\s*gea::embedded::ui::Property::ScaleY,\s*1250\)/)
assert.match(generated, /registerStaticCustomColorRule\([^;]*"zoom",\s*"--ink",\s*17,\s*34,\s*51,\s*255/)
assert.match(generated, /registerStaticCustomLengthRule\([^;]*"zoom",\s*"--gap",\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*8\}/)
assert.match(generated, /registerStaticCustomLengthRule\([^;]*"zoom",\s*"--panel-w",\s*\{gea::embedded::ui::StaticStyleLengthUnit::Percent,\s*72\}/)
assert.match(generated, /registerStaticColorVarRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleColorProperty::Color,\s*"--ink",\s*false,\s*0,\s*0,\s*0,\s*255/)
assert.match(generated, /registerStaticColorVarRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleColorProperty::BackgroundColor,\s*"--paper",\s*true,\s*171,\s*205,\s*239,\s*255/)
assert.match(generated, /registerStaticLengthExpression\(gea::embedded::ui::StaticStyleLengthExpressionKind::Var,[^;]*"--gap",\s*false\)/)
assert.match(generated, /registerStaticLengthExpression\(gea::embedded::ui::StaticStyleLengthExpressionKind::Var,[^;]*"--panel-w",\s*true\)/)
assert.match(generated, /registerStaticLengthSpecRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleLengthProperty::Gap,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Expression,\s*static_cast<float>\(gea_css_len_expr_\d+\)\}/)
assert.match(generated, /registerStaticLengthSpecRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleLengthProperty::Width,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Expression,\s*static_cast<float>\(gea_css_len_expr_\d+\)\}/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"zoom"[^;]*gea::embedded::ui::Property::BorderWidth,\s*0/)
assert.match(generated, /registerStaticPropertyGroupRule\([^;]*"zoom"[^;]*gea::embedded::ui::Property::BorderLeftWidth,\s*0/)
assert.match(generated, /registerStaticLengthSpecRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleLengthProperty::BorderBottomWidth,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*1\}/)
assert.match(generated, /registerStaticColorRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleColorProperty::BorderBottom,\s*255,\s*255,\s*255,\s*64/)
assert.match(generated, /registerStaticLengthSpecRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleLengthProperty::BorderTopWidth,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*2\}/)
assert.match(generated, /registerStaticColorVarRule\([^;]*"zoom",\s*gea::embedded::ui::StaticStyleColorProperty::BorderTop,\s*"--ink",\s*false,\s*0,\s*0,\s*0,\s*255/)
assert.match(generated, /registerStaticFilterBlurRule\([^;]*"zoom",\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*2\}/)
assert.match(generated, /registerStaticBackgroundFullRule\([^;]*"scene"[^;]*"--base-1"[^;]*"--base-2"[^;]*"--base-3"/s)
assert.match(generated, /registerStaticBoxShadowNoneRule\([^;]*"scene"/)
assert.match(generated, /registerStaticGridTemplateRule\([^;]*"scene",\s*gea::embedded::ui::StaticStyleGridTemplateProperty::Columns[^;]*\{2,\s*1[^;]*\{0,\s*0[^;]*\{1,\s*0,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*19\}/s)
assert.match(generated, /registerStaticLengthSpecRule\([^;]*"scene",\s*gea::embedded::ui::StaticStyleLengthProperty::MaskImage,\s*\{gea::embedded::ui::StaticStyleLengthUnit::Px,\s*15\}/)
assert.match(generated, /registerStaticLengthRule\([^;]*"auto-box",\s*gea::embedded::ui::StaticStyleLengthProperty::Width,\s*gea::embedded::ui::StaticStyleLengthUnit::Auto,\s*0/)
assert.match(generated, /registerStaticLengthRule\([^;]*"auto-box",\s*gea::embedded::ui::StaticStyleLengthProperty::Height,\s*gea::embedded::ui::StaticStyleLengthUnit::Auto,\s*0/)
assert.match(generated, /registerStaticColorVarKeyframeRule\("ink",\s*0,\s*gea::embedded::ui::StaticStyleColorProperty::Color,\s*"--ink",\s*false,\s*0,\s*0,\s*0,\s*255\)/)
assert.match(generated, /registerStaticColorVarKeyframeRule\("ink",\s*1000,\s*gea::embedded::ui::StaticStyleColorProperty::Color,\s*"--paper",\s*true,\s*171,\s*205,\s*239,\s*255\)/)
assert.doesNotMatch(generated, /registerStaticRule\("spin",\s*"rotate"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"scale"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"color",\s*"var/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"background-color",\s*"var/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"border"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"border-left"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"border-bottom"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"border-top"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"filter"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"flex-shrink"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"gap"/)
assert.doesNotMatch(generated, /registerStaticRule\("zoom",\s*"width"/)
assert.doesNotMatch(generated, /registerStaticRule\("scene",\s*"background"/)
assert.doesNotMatch(generated, /registerStaticRule\("scene",\s*"box-shadow"/)
assert.doesNotMatch(generated, /registerStaticRule\("scene",\s*"grid-template-columns"/)
assert.doesNotMatch(generated, /registerStaticRule\("scene",\s*"mask-image"/)
assert.doesNotMatch(generated, /registerStaticRule\("auto-box",\s*"width"/)
assert.doesNotMatch(generated, /registerStaticRule\("auto-box",\s*"height"/)
assert.doesNotMatch(generated, /registerStaticKeyframeRule\("turn",\s*0,\s*"rotate"/)
assert.doesNotMatch(generated, /registerStaticKeyframeRule\("grow",\s*0,\s*"scale"/)
assert.doesNotMatch(generated, /registerStaticKeyframeRule\("ink",\s*0,\s*"color"/)

execFileSync(process.execPath, [
  path.join(repoRoot, 'packages/core/scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir,
  '--entry', 'index.ts',
  '--out-dir', tapeOutDir,
  '--gea-embedded-compat',
  '--gea-ir-backend',
  '--allow-any',
], { cwd: repoRoot, stdio: 'pipe' })

const tapeGenerated = fs.readFileSync(path.join(tapeOutDir, 'gea-style-registration.cppfrag'), 'utf8')
assert.match(tapeGenerated, /enum class __GeaStaticCssTapeKind/)
assert.match(tapeGenerated, /const char \* const __gea_static_css_tape_strings\[\]/)
assert.match(tapeGenerated, /struct __GeaStaticCssCustomLengthOp/)
assert.match(tapeGenerated, /struct __GeaStaticCssColorVarOp/)
assert.match(tapeGenerated, /__gea_register_static_css_tape\(\s*\{/)
assert.match(tapeGenerated, /__GeaStaticCssTapeKind::CustomLength/)
assert.match(tapeGenerated, /"--gap"/)
assert.match(tapeGenerated, /\{8,\s*\d+,\s*0,\s*\d+,\s*static_cast<std::uint8_t>\(gea::embedded::ui::StaticStyleSelectorKind::Class\),\s*static_cast<std::uint8_t>\(gea::embedded::ui::StaticStyleLengthUnit::Px\)\}/)
assert.match(tapeGenerated, /__GeaStaticCssTapeKind::LengthSpec/)
assert.match(tapeGenerated, /StaticStyleLengthProperty::Gap/)
assert.match(tapeGenerated, /__GeaStaticCssTapeKind::ColorVar/)
assert.match(tapeGenerated, /"--ink"/)
assert.match(tapeGenerated, /static_cast<std::uint16_t>\(gea::embedded::ui::StaticStyleColorProperty::Color\)/)
assert.match(tapeGenerated, /registerStaticLengthRule\([^;]*"auto-box",\s*gea::embedded::ui::StaticStyleLengthProperty::Width,\s*gea::embedded::ui::StaticStyleLengthUnit::Auto,\s*0/)
assert.match(tapeGenerated, /registerStaticLengthRule\([^;]*"auto-box",\s*gea::embedded::ui::StaticStyleLengthProperty::Height,\s*gea::embedded::ui::StaticStyleLengthUnit::Auto,\s*0/)
assert.doesNotMatch(tapeGenerated, /registerStaticRule\("zoom",\s*"gap"/)
