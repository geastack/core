import type { ClassValue } from '@geastack/core'
import './ProgressBar.css'

// A horizontal progress track composed from plain views, so it renders the same
// on pixel targets, where the native <progress> tag is drawn as an empty box,
// and on desktop renderers. The app owns `value`; the bar only draws it.
// The caller's `class` lands on the root; the bar's own parts use fixed
// `progress-bar__*` classes so callers restyle them with descendant selectors
// instead of the component merging two ClassValues. There is no `style`
// prop: geatsc has no rule for forwarding a Style object through a function
// component, so size and place the bar through `class`.

export interface ProgressBarProps {
  value?: number
  min?: number
  max?: number
  indeterminate?: boolean
  reducedMotion?: boolean
  label?: string
  id?: string
  class?: ClassValue
}

// The negated comparisons also send NaN to 0, so a bad value never reaches the
// layout as a NaN width.
export function progressFraction(value: number, min: number, max: number): number {
  const range = max - min
  if (!(range > 0)) return 0
  const fraction = (value - min) / range
  if (!(fraction > 0)) return 0
  if (fraction > 1) return 1
  return fraction
}

function fillWidth(value: number, min: number, max: number, indeterminate: boolean, reducedMotion: boolean): `${number}%` {
  if (indeterminate) return reducedMotion ? '100%' : '30%'
  return `${Math.round(progressFraction(value, min, max) * 1000) / 10}%`
}

export default function ProgressBar({
  value,
  min,
  max,
  indeterminate,
  reducedMotion,
  label,
  id,
  class: cls
}: ProgressBarProps) {
  const lo = min === undefined ? 0 : min
  const hi = max === undefined ? 100 : max
  const busy = indeterminate === true
  const still = reducedMotion === true
  const text = label === undefined ? '' : label
  return (
    <div id={id} class={cls}>
      <div class="progress-bar__track">
        <div
          class={{
            'progress-bar__fill': true,
            'progress-bar__fill--indeterminate': busy && !still,
            'progress-bar__fill--static': busy && still
          }}
          style={{ width: fillWidth(value === undefined ? 0 : value, lo, hi, busy, still) }}
        ></div>
      </div>
      <span class="progress-bar__label" style={{ display: text.length > 0 ? 'block' : 'none' }}>
        {text}
      </span>
    </div>
  )
}
