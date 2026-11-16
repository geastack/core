import type { ClassValue } from '@geastack/core'
import './InlineAlert.css'

// A persistent message inside a layout, composed from plain views and native
// <button> elements so it renders on pixel targets and desktop renderers
// alike. The severity is spelled out in a text label as well as the colors,
// so it does not rely on color alone; an empty label falls back to the
// default so that cue is never blank. The app owns visibility: the dismiss
// button only reports onDismiss, and the app removes the alert. The root
// carries role="status" (role="alert" for errors) for the browser target;
// native targets ignore the role.
// The caller's `class` lands on the root; the alert's own parts use fixed
// `inline-alert__*` classes. There is no `style` prop: geatsc has no rule for
// forwarding a Style object through a function component.

export type InlineAlertVariant = 'info' | 'success' | 'warning' | 'error'

export interface InlineAlertProps {
  variant?: InlineAlertVariant
  title?: string
  message?: string
  severityLabel?: string
  actionLabel?: string
  onAction?: () => void
  dismissible?: boolean
  dismissLabel?: string
  onDismiss?: () => void
  id?: string
  class?: ClassValue
}

export function inlineAlertSeverityLabel(variant: InlineAlertVariant): string {
  if (variant === 'success') return 'Success'
  if (variant === 'warning') return 'Warning'
  if (variant === 'error') return 'Error'
  return 'Info'
}

export default function InlineAlert({
  variant,
  title,
  message,
  severityLabel,
  actionLabel,
  onAction,
  dismissible,
  dismissLabel,
  onDismiss,
  id,
  class: cls
}: InlineAlertProps) {
  const kind = variant === undefined ? 'info' : variant
  const severity = severityLabel === undefined || severityLabel.length === 0 ? inlineAlertSeverityLabel(kind) : severityLabel
  const heading = title === undefined ? '' : title
  const text = message === undefined ? '' : message
  const action = actionLabel === undefined ? '' : actionLabel
  const canDismiss = dismissible === true
  const closeText = dismissLabel === undefined || dismissLabel.length === 0 ? 'Dismiss' : dismissLabel
  return (
    <div id={id} class={cls} role={kind === 'error' ? 'alert' : 'status'}>
      <div
        class={{
          'inline-alert': true,
          'inline-alert--info': kind === 'info',
          'inline-alert--success': kind === 'success',
          'inline-alert--warning': kind === 'warning',
          'inline-alert--error': kind === 'error'
        }}
      >
        <div class="inline-alert__body">
          <span class="inline-alert__severity">{severity}</span>
          {heading.length > 0 ? <span class="inline-alert__title">{heading}</span> : null}
          {text.length > 0 ? <span class="inline-alert__message">{text}</span> : null}
        </div>
        {action.length > 0 || canDismiss ? (
          <div class="inline-alert__actions">
            {action.length > 0 ? (
              <button class="inline-alert__action" onClick={() => onAction?.()}>
                {action}
              </button>
            ) : null}
            {canDismiss ? (
              <button class="inline-alert__dismiss" onClick={() => onDismiss?.()}>
                {closeText}
              </button>
            ) : null}
          </div>
        ) : null}
      </div>
    </div>
  )
}
