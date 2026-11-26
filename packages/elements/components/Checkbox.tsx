import type { CheckboxProps, Style } from '@geastack/core'

export function Checkbox({
  checked = false,
  indeterminate = false,
  disabled = false,
  label,
  class: customClass,
  style,
  onChange,
  onPress,
  onClick,
}: CheckboxProps) {
  if (typeof label !== 'string' || label.trim().length === 0) {
    throw new Error('Checkbox requires a non-empty label.')
  }

  const handleClick = (event: Parameters<NonNullable<CheckboxProps['onClick']>>[0]) => {
    if (disabled) return

    onChange?.(indeterminate ? true : !checked)
    onPress?.(event)
    onClick?.(event)
  }

  const active = checked || indeterminate
  const rootStyle: Style = {
    display: 'flex',
    alignItems: 'center',
    gap: 8,
    opacity: disabled ? 0.55 : 1,
    ...style,
  }
  const boxStyle: Style = {
    width: 18,
    height: 18,
    flex: 0,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 2,
    borderColor: active ? '#2563eb' : '#64748b',
    borderRadius: 4,
    backgroundColor: active ? '#2563eb' : 'transparent',
    color: '#ffffff',
    fontSize: 12,
  }

  // One click listener handles both pointer and touch-generated click events.
  // Binding both onPress and onClick here would dispatch the same activation twice.
  return (
    <div class={customClass} style={rootStyle} onClick={handleClick}>
      <span style={boxStyle}>{indeterminate ? '-' : checked ? 'x' : ''}</span>
      <span>{label}</span>
    </div>
  )
}
