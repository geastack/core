import type { ClassValue, KeyEvent, PressHandler, Style } from '@geastack/core'

export interface CheckboxProps {
  checked?: boolean
  indeterminate?: boolean
  disabled?: boolean
  label?: string
  class?: ClassValue
  style?: Style
  onChange?: (checked: boolean) => void
  onPress?: PressHandler
  onClick?: PressHandler
}

export function Checkbox(props: CheckboxProps): any