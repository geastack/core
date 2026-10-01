import type { StyleUsageObserver } from './analyze-renderer.js'

// Bounds are build facts. Keep the unit separate: numeric/native-style values
// are raw pixels, whereas authored px lengths scale with the target CSS DPR.
const families = ['padding', 'gap', 'border', 'radius', 'font', 'line-height', 'flex'] as const
type Family = typeof families[number]
type Bound = { raw: number; px: number; unknown: boolean }
const defaults = /^(?:initial|inherit|unset|revert|revert-layer)$/
const number = /^[+]?(?:\d+(?:\.\d*)?|\.\d+)$/

export function cssRangeObserver(features: Set<string>): StyleUsageObserver & { finish(): void } {
  const bounds = new Map<Family, Bound>(families.map(name => [name, { raw: name === 'flex' ? 1 : 0, px: name === 'font' ? 16 : 0, unknown: false }]))
  let opaque = false
  let unknownCircle = false
  let lineMultiplier = 0
  const unknown = (...names: Family[]): void => { for (const name of names) bounds.get(name)!.unknown = true }
  const length = (name: Family, value: string | undefined, count = 1): void => {
    if (value !== undefined && defaults.test(value)) return
    const parts = value?.split(/\s+/)
    if (!parts?.length || parts.length > count) { unknown(name); return }
    for (const part of parts) {
      const match = part.match(/^([+]?(?:\d+(?:\.\d*)?|\.\d+))(px)?$/)
      if (!match || !Number.isFinite(Number(match[1]))) { unknown(name); continue }
      const unit = match[2] ? 'px' : 'raw'
      bounds.get(name)![unit] = Math.max(bounds.get(name)![unit], Math.ceil(Number(match[1])))
    }
  }
  return {
    unknown(): void { opaque = true },
    unknownCircleBounds(): void { unknownCircle = true },
    selector(css): void { if (/@(?:-webkit-)?keyframes\b|@property\b/i.test(css)) opaque = true },
    property(name, value): void {
      if (!name) { opaque = true; return }
      name = name.replace(/[A-Z]/g, letter => `-${letter.toLowerCase()}`).toLowerCase()
      value = value?.trim().toLowerCase().replace(/\s*!important$/, '')
      if (name === 'all' || /^(?:-webkit-)?(?:animation|transition)(?:-|$)/.test(name)) { opaque = true; return }
      if (/^padding(?:-|$)/.test(name)) length('padding', value, 4)
      if (/^(?:grid-)?(?:(?:row|column)-)?gap$/.test(name)) length('gap', value === 'normal' ? '0' : value, 2)
      if (/^border(?:-(?:top-left|top-right|bottom-left|bottom-right|start-start|start-end|end-start|end-end))?-radius$/.test(name)) length('radius', value, 4)
      if (/^border(?:-(?:top|right|bottom|left|block|inline)(?:-(?:start|end))?)?-width$/.test(name)) {
        length('border', value?.replace(/\bthin\b/g, '1px').replace(/\bmedium\b/g, '3px').replace(/\bthick\b/g, '5px'), 4)
      }
      if (/^border(?:-(?:top|right|bottom|left|block|inline)(?:-(?:start|end))?)?$/.test(name)) {
        if (value === undefined || /[()]/.test(value)) unknown('border')
        else if (!defaults.test(value)) {
          const tokens = value.split(/\s+/)
          const widths = tokens.filter(token => /^(?:[+.-]?\d|thin$|medium$|thick$)/.test(token))
          if (widths.length > 1) unknown('border')
          else length('border', widths[0]?.replace(/^thin$/, '1px').replace(/^medium$/, '3px').replace(/^thick$/, '5px') ?? (tokens.some(t => /^(?:none|hidden)$/.test(t)) ? '0' : '3px'))
        }
      }
      if (name === 'font-size') length('font', value)
      if (name === 'font' && (value === undefined || !defaults.test(value))) unknown('font', 'line-height')
      if (name === 'line-height' && value !== 'normal' && !(value !== undefined && defaults.test(value))) {
        if (value !== undefined && number.test(value)) lineMultiplier = Math.max(lineMultiplier, Math.ceil(Number(value)))
        else length('line-height', value)
      }
      if (name === 'flex-grow' || name === 'flex-shrink') {
        if (value === undefined || (!defaults.test(value) && !number.test(value))) unknown('flex')
        else if (!defaults.test(value)) length('flex', value)
      }
      if (name === 'flex' && !/^(?:none|auto|initial|inherit|unset|revert|revert-layer)$/.test(value ?? '')) {
        const parts = value?.split(/\s+/)
        if (!parts?.length || parts.length > 3 || !number.test(parts[0])) unknown('flex')
        else {
          length('flex', parts[0])
          if (parts.length > 1 && number.test(parts[1])) length('flex', parts[1])
          else if (parts.length > 2 || (parts.length === 2 && !/^(?:auto|content|[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|%))$/.test(parts[1]))) unknown('flex')
        }
      }
    },
    finish(): void {
      features.add('css-ranges-v1')
      features.add('css-circle-cache-v1')
      // Explicit unknown survives feature unions from separately analyzed roots.
      if (opaque || unknownCircle || bounds.get('radius')!.unknown) features.add('css-circle-cache-unbounded')
      if (opaque) { features.add('css-ranges-unknown'); return }
      const font = bounds.get('font')!, line = bounds.get('line-height')!
      if (lineMultiplier) {
        line.unknown ||= font.unknown || !Number.isFinite(lineMultiplier)
        line.raw = Math.max(line.raw, font.raw * lineMultiplier)
        line.px = Math.max(line.px, font.px * lineMultiplier)
      }
      for (const [name, bound] of bounds) if (!bound.unknown) {
        for (const unit of ['raw', 'px'] as const) if (Number.isSafeInteger(bound[unit])) features.add(`css-range-${name}-${unit}-${bound[unit]}`)
      }
    },
  }
}
