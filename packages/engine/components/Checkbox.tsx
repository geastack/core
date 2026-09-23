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

export function Checkbox({
  checked = false,
  indeterminate = false,
  disabled = false,
  label,
  class: cls,
  style,
  onChange,
  onPress,
  onClick,
}: CheckboxProps) {
  const toggle = () => {
    if (disabled) return
    onChange?.(indeterminate ? true : !checked)
  }

  const handlePress = (e: any) => {
    toggle()
    onPress?.(e)
    onClick?.(e)
  }

  const handleKeyDown = (e: KeyEvent) => {
    if (e.keyCode === 32 || e.which === 32) {
      toggle()
    }
  }

  const checkClass = [
    'gea-checkbox',
    disabled && 'gea-checkbox-disabled',
    indeterminate && 'gea-checkbox-indeterminate',
    !indeterminate && checked && 'gea-checkbox-checked',
    !indeterminate && !checked && 'gea-checkbox-unchecked',
    cls,
  ]
    .filter(Boolean)
    .join(' ')

  const mark = indeterminate ? '-' : checked ? '✓' : ''

  return (
    <div
      class={checkClass}
      style={style}
      onPress={handlePress}
      onClick={handlePress}
      onKeyDown={handleKeyDown}
      role="checkbox"
      aria-checked={indeterminate ? 'mixed' : checked}
      aria-disabled={disabled}
      tabIndex={disabled ? -1 : 0}
    >
      <span class="gea-checkbox-mark">{mark}</span>
      {label && <span class="gea-checkbox-label">{label}</span>}
    </div>
  )
}