import type { ClassValue, KeyEvent, PressHandler, Style } from '@geastack/core'

export interface CheckboxProps {
  /** Checked state of the checkbox */
  checked?: boolean
  /** Indeterminate/mixed state of the checkbox */
  indeterminate?: boolean
  /** Whether the checkbox is disabled */
  disabled?: boolean
  /** Optional text label to display beside the checkbox */
  label?: string
  /** Custom class names (string, array, or class map) */
  class?: ClassValue
  /** Custom inline styles */
  style?: Style
  /** Triggered when the checked state changes */
  onChange?: (checked: boolean) => void
  /** Triggered via press/click. Note: Runs through the native click event path */
  onPress?: PressHandler
  /** Triggered via click. Note: Runs through the native click event path */
  onClick?: PressHandler
}

export function Checkbox(props: CheckboxProps): any