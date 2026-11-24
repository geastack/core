import type { ClassValue } from '@geastack/core'

export interface ProgressBarProps {
  /** Current value, clamped to [min, max]. Defaults to 0. */
  value?: number
  /** Defaults to 0. */
  min?: number
  /** Defaults to 100. When max <= min the bar shows empty. */
  max?: number
  /**
   * Show an animated sweep instead of a value. The sweep distance is fixed in
   * pixels when the animation starts, so a bar that changes width while busy
   * keeps the old distance until `indeterminate` is toggled.
   */
  indeterminate?: boolean
  /** With `indeterminate`, show a still, dimmed full-width fill instead of the sweep. */
  reducedMotion?: boolean
  /** Text shown under the track. Hidden when empty. */
  label?: string
  id?: string
  /** Applied to the root; size the bar here. Restyle parts with `.your-class .progress-bar__track`, `__fill`, `__label`. */
  class?: ClassValue
}

/** The clamped position of `value` in [min, max], from 0 to 1. Returns 0 for an empty or inverted range and for NaN. */
export declare function progressFraction(value: number, min: number, max: number): number

/**
 * A progress track composed from plain views.
 *
 * Accessibility: the bar exposes no role or value to assistive technology on
 * any target, and it takes no keyboard input. Put the value in `label` when
 * the user needs to read it.
 */
export default function ProgressBar(props: ProgressBarProps): any
