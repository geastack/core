import type { ClassValue } from '@geastack/core'

export type InlineAlertVariant = 'info' | 'success' | 'warning' | 'error'

export interface InlineAlertProps {
  /** Defaults to `'info'`. Sets the colors and the default severity label. */
  variant?: InlineAlertVariant
  /** Bold heading above the message. Not rendered when empty. */
  title?: string
  /** Body text. Wraps to the alert's width. Not rendered when empty. */
  message?: string
  /**
   * The visible severity text, so the variant does not rely on color alone.
   * Defaults to `inlineAlertSeverityLabel(variant)` (English), also when
   * empty; pass a translation for other languages. On pixel targets the
   * built-in bitmap font draws ASCII only, so any other character, such as
   * Turkish `ü`, `ş` or `ı`, draws as `?` unless the app enables runtime TTF
   * fonts or names the characters in a `--gea-font-charset`.
   */
  severityLabel?: string
  /** Text of the action button. The button is rendered only when this is not empty. */
  actionLabel?: string
  /** Called when the action button is pressed. */
  onAction?: () => void
  /** Show a dismiss button. Defaults to false. */
  dismissible?: boolean
  /** Text of the dismiss button. Defaults to `'Dismiss'`, also when empty; name what it closes when several alerts are on screen. */
  dismissLabel?: string
  /**
   * Called when the dismiss button is pressed. The alert does not hide itself;
   * remove it from the app's template here.
   */
  onDismiss?: () => void
  /** Applied to the root. */
  id?: string
  /**
   * Applied to the root; size and place the alert here. Restyle parts with
   * descendant selectors such as `.your-class .inline-alert__title`; the parts
   * are `inline-alert`, `inline-alert__body`, `__severity`, `__title`,
   * `__message`, `__actions`, `__action` and `__dismiss`. Restyle a variant
   * through its modifier, `.your-class .inline-alert--error`: a rule on
   * `.your-class .inline-alert` overrides every variant's colors.
   */
  class?: ClassValue
}

/** The default English severity text for a variant: `'Info'`, `'Success'`, `'Warning'` or `'Error'`. */
export declare function inlineAlertSeverityLabel(variant: InlineAlertVariant): string

/**
 * A persistent message inside a layout, composed from plain views and native
 * `<button>` elements. Buttons that do not fit on one line wrap to the next.
 *
 * Accessibility: on the browser target the root has `role="status"`, or
 * `role="alert"` for errors, so a screen reader announces it. Native targets
 * ignore the role and expose no status to assistive technology. The action
 * and dismiss controls are native buttons.
 *
 * Layout: the stripe is on the left edge; right-to-left layouts are not
 * handled.
 */
export default function InlineAlert(props: InlineAlertProps): any
