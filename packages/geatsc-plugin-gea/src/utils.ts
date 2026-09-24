import type { PluginOptionMap } from './types.js'

export function boolOption(options: PluginOptionMap, name: string): boolean {
  const value = options[name]
  return value === '1' || value === 'true' || value === 'yes'
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

export function cppString(value: string): string {
  return JSON.stringify(value)
}

export function cssPropertyName(value: string): string {
  if (value.startsWith('--')) return value
  return value.replace(/[A-Z]/g, (char) => `-${char.toLowerCase()}`)
}

export function styleDeclarationEnumForPropertyName(propertyName: string): string | null {
  if (propertyName.startsWith('--')) return null
  return STYLE_DECLARATION_ENUM_BY_PROPERTY.get(propertyName) ?? null
}

export function isBorderWidthStylePropertyName(propertyName: string): boolean {
  return /^border-(?:(?:top|right|bottom|left)-)?width$/.test(propertyName)
}

export function isPxLengthStylePropertyName(propertyName: string): boolean {
  return PX_LENGTH_STYLE_PROPERTIES.has(propertyName)
}

export function stylePercentPropertyEnumForPropertyName(propertyName: string): string | null {
  return STYLE_PERCENT_PROPERTY_ENUM_BY_PROPERTY.get(propertyName) ?? null
}

export function styleRawNumberPropertyEnumsForPropertyName(propertyName: string): string[] | null {
  return STYLE_RAW_NUMBER_PROPERTY_ENUMS_BY_PROPERTY.get(propertyName) ?? null
}

export interface StylePropertyValue {
  property: string
  value: number
}

export function styleKeywordPropertyValuesForPropertyName(propertyName: string, rawValue: string): StylePropertyValue[] | null {
  const value = rawValue.trim().toLowerCase()
  if (propertyName === 'display') return keywordValue('Display', DISPLAY_KEYWORDS, value)
  if (propertyName === 'flex-direction') return keywordValue('FlexDirection', FLEX_DIRECTION_KEYWORDS, value)
  if (propertyName === 'flex-wrap') return keywordValue('FlexWrap', FLEX_WRAP_KEYWORDS, value)
  if (propertyName === 'justify-content') return keywordValue('JustifyContent', FLEX_ALIGN_KEYWORDS, value)
  if (propertyName === 'align-items') return keywordValue('AlignItems', SELF_ALIGN_KEYWORDS, value)
  if (propertyName === 'justify-items') return keywordValue('JustifyItems', SELF_ALIGN_KEYWORDS, value)
  if (propertyName === 'align-content') return keywordValue('AlignContent', FLEX_ALIGN_KEYWORDS, value)
  if (propertyName === 'align-self') return keywordValue('AlignSelf', ALIGN_SELF_KEYWORDS, value)
  if (propertyName === 'place-items') {
    const align = SELF_ALIGN_KEYWORDS.get(value)
    return align === undefined
      ? null
      : [
          { property: 'AlignItems', value: align },
          { property: 'JustifyItems', value: align },
        ]
  }
  if (propertyName === 'position') return keywordValue('Position', POSITION_KEYWORDS, value)
  if (propertyName === 'text-align') return keywordValue('TextAlign', TEXT_ALIGN_KEYWORDS, value)
  if (propertyName === 'white-space') return keywordValue('WhiteSpace', WHITE_SPACE_KEYWORDS, value)
  if (propertyName === 'text-overflow') return keywordValue('TextOverflow', TEXT_OVERFLOW_KEYWORDS, value)
  if (propertyName === 'backface-visibility') return keywordValue('Backface', BACKFACE_KEYWORDS, value)
  if (propertyName === 'pointer-events') return keywordValue('PointerEvents', POINTER_EVENTS_KEYWORDS, value)
  if (propertyName === 'overflow') return keywordValue('Overflow', OVERFLOW_KEYWORDS, value)
  if (propertyName === 'overflow-x') return keywordValue('OverflowX', OVERFLOW_KEYWORDS, value)
  if (propertyName === 'overflow-y') return keywordValue('OverflowY', OVERFLOW_KEYWORDS, value)
  if (propertyName === 'text-decoration' || propertyName === 'text-decoration-line') {
    return keywordValue('TextDecoration', TEXT_DECORATION_KEYWORDS, value)
  }
  if (propertyName === 'text-transform') return keywordValue('TextTransform', TEXT_TRANSFORM_KEYWORDS, value)
  return null
}

export function styleKeywordPropertiesForPropertyName(propertyName: string): string[] | null {
  return STYLE_KEYWORD_PROPERTIES_BY_PROPERTY.get(propertyName) ?? null
}

export interface OpaqueCssColor {
  r: number
  g: number
  b: number
}

export type OpaqueColorStyleTarget =
  | 'background'
  | 'color'
  | 'active-background'
  | 'border-color'
  | 'border-top-color'
  | 'border-right-color'
  | 'border-bottom-color'
  | 'border-left-color'

export function opaqueCssHexColor(rawValue: string): OpaqueCssColor | null {
  const text = rawValue.trim()
  const match = text.match(/^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/)
  if (!match) return null
  const hex = match[1]
  if (hex.length === 3) {
    return {
      r: parseInt(hex[0] + hex[0], 16),
      g: parseInt(hex[1] + hex[1], 16),
      b: parseInt(hex[2] + hex[2], 16),
    }
  }
  return {
    r: parseInt(hex.slice(0, 2), 16),
    g: parseInt(hex.slice(2, 4), 16),
    b: parseInt(hex.slice(4, 6), 16),
  }
}

export function styleOpaqueColorTargetForPropertyName(propertyName: string): OpaqueColorStyleTarget | null {
  if (propertyName === 'color') return 'color'
  if (propertyName === 'background' || propertyName === 'background-color') return 'background'
  if (propertyName === 'active-background' || propertyName === 'active-background-color') return 'active-background'
  if (
    propertyName === 'border-color' ||
    propertyName === 'border-top-color' ||
    propertyName === 'border-right-color' ||
    propertyName === 'border-bottom-color' ||
    propertyName === 'border-left-color'
  ) {
    return propertyName
  }
  return null
}

export function nativeStyleColorExpression(color: OpaqueCssColor): string {
  return `gea::framework::graphics::pixel::nativeStyleValue(${color.r}, ${color.g}, ${color.b})`
}

function keywordValue(property: string, map: Map<string, number>, value: string): StylePropertyValue[] | null {
  const mapped = map.get(value)
  return mapped === undefined ? null : [{ property, value: mapped }]
}

const STYLE_DECLARATION_ENUM_BY_PROPERTY = new Map<string, string>([
  ['color-scheme', 'Ignored'],
  ['background-position', 'BackgroundPosition'],
  ['background-repeat', 'BackgroundRepeat'],
  ['background-attachment', 'BackgroundAttachment'],
  ['background-origin', 'BackgroundOrigin'],
  ['box-sizing', 'BoxSizing'],
  ['float', 'Float'],
  ['clear', 'Clear'],
  ['margin-trim', 'MarginTrim'],
  ['writing-mode', 'WritingMode'],
  ['direction', 'Direction'],
  ['flex-flow', 'FlexFlow'],
  ['row-gap', 'RowGap'],
  ['column-gap', 'ColumnGap'],
  ['font', 'Font'],
  ['grid-column', 'Ignored'],
  ['isolation', 'Ignored'],
  ['letter-spacing', 'Ignored'],
  ['object-fit', 'ObjectFit'],
  ['outline', 'Ignored'],
  ['scroll-snap-align', 'Ignored'],
  ['scroll-snap-type', 'Ignored'],
  ['scrollbar-width', 'Ignored'],
  ['text-shadow', 'Ignored'],
  ['transform-style', 'Ignored'],
  ['transition', 'Ignored'],
  ['cursor', 'Ignored'],
  ['-webkit-tap-highlight-color', 'Ignored'],
  ['animation', 'Animation'],
  ['display', 'Display'],
  ['flex-direction', 'FlexDirection'],
  ['flex-wrap', 'FlexWrap'],
  ['order', 'Order'],
  ['justify-content', 'JustifyContent'],
  ['align-items', 'AlignItems'],
  ['justify-items', 'JustifyItems'],
  ['align-content', 'AlignContent'],
  ['align-self', 'AlignSelf'],
  ['place-items', 'PlaceItems'],
  ['place-content', 'PlaceContent'],
  ['place-self', 'PlaceSelf'],
  ['grid-template-columns', 'GridTemplateColumns'],
  ['grid-template-rows', 'GridTemplateRows'],
  ['content', 'Content'],
  ['gap', 'Gap'],
  ['width', 'Width'],
  ['height', 'Height'],
  ['min-width', 'MinWidth'],
  ['min-height', 'MinHeight'],
  ['max-width', 'MaxWidth'],
  ['max-height', 'MaxHeight'],
  ['flex', 'Flex'],
  ['flex-grow', 'FlexGrow'],
  ['flex-shrink', 'FlexShrink'],
  ['flex-basis', 'FlexBasis'],
  ['padding', 'Padding'],
  ['padding-top', 'PaddingTop'],
  ['padding-right', 'PaddingRight'],
  ['padding-bottom', 'PaddingBottom'],
  ['padding-left', 'PaddingLeft'],
  ['margin', 'Margin'],
  ['margin-top', 'MarginTop'],
  ['margin-right', 'MarginRight'],
  ['margin-bottom', 'MarginBottom'],
  ['margin-left', 'MarginLeft'],
  ['position', 'Position'],
  ['inset', 'Inset'],
  ['top', 'Top'],
  ['right', 'Right'],
  ['bottom', 'Bottom'],
  ['left', 'Left'],
  ['z-index', 'ZIndex'],
  ['active-background-color', 'ActiveBackgroundColor'],
  ['active-background', 'ActiveBackgroundColor'],
  ['background-color', 'BackgroundColor'],
  ['background', 'Background'],
  ['background-image', 'BackgroundImage'],
  ['background-clip', 'BackgroundClip'],
  ['contain', 'Contain'],
  ['background-size', 'BackgroundSize'],
  ['color', 'Color'],
  ['opacity', 'Opacity'],
  ['border-color', 'BorderColor'],
  ['border', 'Border'],
  ['border-width', 'BorderWidth'],
  ['border-top', 'BorderTop'],
  ['border-right', 'BorderRight'],
  ['border-bottom', 'BorderBottom'],
  ['border-left', 'BorderLeft'],
  ['border-top-width', 'BorderTopWidth'],
  ['border-right-width', 'BorderRightWidth'],
  ['border-bottom-width', 'BorderBottomWidth'],
  ['border-left-width', 'BorderLeftWidth'],
  ['border-top-color', 'BorderTopColor'],
  ['border-right-color', 'BorderRightColor'],
  ['border-bottom-color', 'BorderBottomColor'],
  ['border-left-color', 'BorderLeftColor'],
  ['border-radius', 'BorderRadius'],
  ['border-top-left-radius', 'BorderTopLeftRadius'],
  ['border-top-right-radius', 'BorderTopRightRadius'],
  ['border-bottom-right-radius', 'BorderBottomRightRadius'],
  ['border-bottom-left-radius', 'BorderBottomLeftRadius'],
  ['font-family', 'FontFamily'],
  ['font-size', 'FontSize'],
  ['font-weight', 'FontWeight'],
  ['line-height', 'LineHeight'],
  ['text-align', 'TextAlign'],
  ['text-decoration', 'TextDecoration'],
  ['text-decoration-line', 'TextDecoration'],
  ['text-transform', 'TextTransform'],
  ['white-space', 'WhiteSpace'],
  ['text-overflow', 'TextOverflow'],
  ['backface-visibility', 'BackfaceVisibility'],
  ['pointer-events', 'PointerEvents'],
  ['overflow', 'Overflow'],
  ['overflow-x', 'OverflowX'],
  ['overflow-y', 'OverflowY'],
  ['mask-image', 'MaskImage'],
  ['-webkit-mask-image', 'MaskImage'],
  ['transform', 'Transform'],
  ['rotate', 'Rotate'],
  ['scale', 'Scale'],
  ['filter', 'Filter'],
  ['box-shadow', 'BoxShadow'],
  ['transform-origin', 'TransformOrigin'],
  ['perspective', 'Perspective'],
  ['perspective-origin', 'PerspectiveOrigin'],
])

const PX_LENGTH_STYLE_PROPERTIES = new Set<string>([
  'gap',
  'width',
  'height',
  'min-width',
  'min-height',
  'max-width',
  'max-height',
  'padding',
  'padding-top',
  'padding-right',
  'padding-bottom',
  'padding-left',
  'margin',
  'margin-top',
  'margin-right',
  'margin-bottom',
  'margin-left',
  'top',
  'right',
  'bottom',
  'left',
  'border-width',
  'border-top-width',
  'border-right-width',
  'border-bottom-width',
  'border-left-width',
  'border-radius',
  'border-top-left-radius',
  'border-top-right-radius',
  'border-bottom-right-radius',
  'border-bottom-left-radius',
  'font-size',
  'perspective',
  'translate-x',
  'translate-y',
])

const STYLE_PERCENT_PROPERTY_ENUM_BY_PROPERTY = new Map<string, string>([
  ['width', 'WidthPercent'],
  ['height', 'HeightPercent'],
  ['top', 'TopPercent'],
  ['right', 'RightPercent'],
  ['bottom', 'BottomPercent'],
  ['left', 'LeftPercent'],
])

const STYLE_RAW_NUMBER_PROPERTY_ENUMS_BY_PROPERTY = new Map<string, string[]>([
  ['gap', ['Gap']],
  ['width', ['Width']],
  ['height', ['Height']],
  ['min-width', ['MinWidth']],
  ['min-height', ['MinHeight']],
  ['max-width', ['MaxWidth']],
  ['max-height', ['MaxHeight']],
  ['padding', ['PaddingTop', 'PaddingRight', 'PaddingBottom', 'PaddingLeft']],
  ['padding-top', ['PaddingTop']],
  ['padding-right', ['PaddingRight']],
  ['padding-bottom', ['PaddingBottom']],
  ['padding-left', ['PaddingLeft']],
  ['margin', ['MarginTop', 'MarginRight', 'MarginBottom', 'MarginLeft']],
  ['margin-top', ['MarginTop']],
  ['margin-right', ['MarginRight']],
  ['margin-bottom', ['MarginBottom']],
  ['margin-left', ['MarginLeft']],
  ['top', ['Top']],
  ['right', ['Right']],
  ['bottom', ['Bottom']],
  ['left', ['Left']],
  ['z-index', ['ZIndex']],
  ['border-width', ['BorderWidth']],
  ['border-top-width', ['BorderTopWidth']],
  ['border-right-width', ['BorderRightWidth']],
  ['border-bottom-width', ['BorderBottomWidth']],
  ['border-left-width', ['BorderLeftWidth']],
  ['border-radius', ['BorderRadiusTopLeft', 'BorderRadiusTopRight', 'BorderRadiusBottomRight', 'BorderRadiusBottomLeft']],
  ['border-top-left-radius', ['BorderRadiusTopLeft']],
  ['border-top-right-radius', ['BorderRadiusTopRight']],
  ['border-bottom-right-radius', ['BorderRadiusBottomRight']],
  ['border-bottom-left-radius', ['BorderRadiusBottomLeft']],
  ['font-size', ['FontSize']],
  ['font-weight', ['FontWeight']],
])

const STYLE_KEYWORD_PROPERTIES_BY_PROPERTY = new Map<string, string[]>([
  ['display', ['Display']],
  ['flex-direction', ['FlexDirection']],
  ['flex-wrap', ['FlexWrap']],
  ['justify-content', ['JustifyContent']],
  ['align-items', ['AlignItems']],
  ['justify-items', ['JustifyItems']],
  ['align-content', ['AlignContent']],
  ['align-self', ['AlignSelf']],
  ['place-items', ['AlignItems', 'JustifyItems']],
  ['position', ['Position']],
  ['text-align', ['TextAlign']],
  ['white-space', ['WhiteSpace']],
  ['text-overflow', ['TextOverflow']],
  ['backface-visibility', ['Backface']],
  ['pointer-events', ['PointerEvents']],
  ['overflow', ['Overflow']],
  ['overflow-x', ['OverflowX']],
  ['overflow-y', ['OverflowY']],
  ['text-decoration', ['TextDecoration']],
  ['text-decoration-line', ['TextDecoration']],
  ['text-transform', ['TextTransform']],
])

const DISPLAY_KEYWORDS = new Map<string, number>([
  ['block', 0],
  ['none', 1],
  ['grid', 2],
  ['inline-grid', 2],
  ['flex', 3],
  ['inline-flex', 3],
])

const FLEX_DIRECTION_KEYWORDS = new Map<string, number>([
  ['column', 0],
  ['row', 1],
  ['column-reverse', 2],
  ['row-reverse', 3],
])

const FLEX_WRAP_KEYWORDS = new Map<string, number>([
  ['nowrap', 0],
  ['wrap', 1],
  ['wrap-reverse', 2],
])

const FLEX_ALIGN_KEYWORDS = new Map<string, number>([
  ['stretch', 0],
  ['normal', 0],
  ['flex-start', 6],
  ['start', 7],
  ['center', 1],
  ['flex-end', 2],
  ['end', 8],
  ['space-between', 3],
  ['space-around', 4],
  ['space-evenly', 14],
  ['baseline', 5],
])

const SELF_ALIGN_KEYWORDS = new Map([...FLEX_ALIGN_KEYWORDS].filter(([key]) => !key.startsWith('space-')))

const ALIGN_SELF_KEYWORDS = new Map<string, number>([
  ['auto', -1],
  ...SELF_ALIGN_KEYWORDS,
])

const POSITION_KEYWORDS = new Map<string, number>([
  ['static', 0],
  ['absolute', 1],
  ['relative', 2],
])

const TEXT_ALIGN_KEYWORDS = new Map<string, number>([
  ['left', 0],
  ['start', 0],
  ['center', 1],
  ['right', 2],
  ['end', 2],
])

const WHITE_SPACE_KEYWORDS = new Map<string, number>([
  ['normal', 0],
  ['nowrap', 1],
  ['pre', 2],
  ['pre-wrap', 3],
  ['pre-line', 4],
  ['break-spaces', 5],
])

const TEXT_OVERFLOW_KEYWORDS = new Map<string, number>([
  ['clip', 0],
  ['ellipsis', 1],
])

const BACKFACE_KEYWORDS = new Map<string, number>([
  ['visible', 0],
  ['hidden', 1],
])

const POINTER_EVENTS_KEYWORDS = new Map<string, number>([
  ['auto', 0],
  ['none', 1],
])

const OVERFLOW_KEYWORDS = new Map<string, number>([
  ['visible', 0],
  ['hidden', 1],
  ['scroll', 2],
  ['auto', 2],
])

const TEXT_DECORATION_KEYWORDS = new Map<string, number>([
  ['none', 0],
  ['underline', 1],
  ['line-through', 2],
  ['strikethrough', 2],
])

const TEXT_TRANSFORM_KEYWORDS = new Map<string, number>([
  ['none', 0],
  ['uppercase', 1],
  ['lowercase', 2],
  ['capitalize', 3],
])

// Module-level runtime numeric constants follow the SCREAMING_SNAKE_CASE
// convention (DISPLAY_W, ITEM_COUNT, ITEM_HEIGHT, MAX_SCROLL, …). They are
// assigned once during module init — before any component mounts — and never
// reassigned, so a template slot that reads one carries no store dependency
// and its value can be resolved eagerly at mount time. The optimized C++
// template renderer relies on this to avoid bailing such components to the
// closure-codegen fallback (whose reactive bindings don't fire an initial
// apply in the native runtime).
export function isRuntimeNumericConstantIdentifier(name: string): boolean {
  return /^[A-Z][A-Z0-9_]*$/.test(name)
}

export function sanitizeCppIdentifier(value: string): string {
  const ident = value.replace(/[^A-Za-z0-9_]/g, (char) => `_x${char.codePointAt(0)!.toString(16)}_`)
  const safe = ident ? (/^[A-Za-z_]/.test(ident) ? ident : `__gea_${ident}`) : 'anonymous'
  return safe === 'main' || cppReservedNames.has(safe) ? `__gea_${safe}` : safe
}

const cppReservedNames = new Set([
  'alignas',
  'alignof',
  'and',
  'and_eq',
  'asm',
  'auto',
  'bitand',
  'bitor',
  'bool',
  'break',
  'case',
  'catch',
  'char',
  'char8_t',
  'char16_t',
  'char32_t',
  'class',
  'compl',
  'concept',
  'const',
  'consteval',
  'constexpr',
  'constinit',
  'const_cast',
  'continue',
  'co_await',
  'co_return',
  'co_yield',
  'decltype',
  'default',
  'delete',
  'do',
  'double',
  'dynamic_cast',
  'else',
  'enum',
  'explicit',
  'export',
  'extern',
  'false',
  'float',
  'for',
  'friend',
  'goto',
  'if',
  'inline',
  'int',
  'long',
  'mutable',
  'namespace',
  'new',
  'noexcept',
  'not',
  'not_eq',
  'nullptr',
  'operator',
  'or',
  'or_eq',
  'private',
  'protected',
  'public',
  'register',
  'reinterpret_cast',
  'requires',
  'return',
  'short',
  'signed',
  'sizeof',
  'static',
  'static_assert',
  'static_cast',
  'struct',
  'switch',
  'template',
  'this',
  'thread_local',
  'throw',
  'true',
  'try',
  'typedef',
  'typeid',
  'typename',
  'typeof',
  'typeof_unqual',
  'union',
  'unsigned',
  'using',
  'virtual',
  'void',
  'volatile',
  'wchar_t',
  'while',
  'xor',
  'xor_eq',
  'y0',
  'y1',
  'yn',
  'j0',
  'j1',
  'jn',
  'fma',
  'gamma',
  'lgamma',
  'tgamma',
  'exp2',
  'exp10',
  'expm1',
  'log2',
  'log10',
  'log1p',
  'cbrt',
  'hypot',
  'erf',
  'erfc',
  'remainder',
  'remquo',
  'nan',
  'div',
  'ldiv',
  'lldiv',
  'time',
  'clock',
  'rand',
  'log',
  'logb',
  'exp',
  'sqrt',
  'pow',
  'sin',
  'cos',
  'tan',
  'asin',
  'acos',
  'atan',
  'atan2',
  'sinh',
  'cosh',
  'tanh',
  'asinh',
  'acosh',
  'atanh',
  'ceil',
  'floor',
  'round',
  'trunc',
  'fabs',
  'abs',
  'fmod',
  'fmin',
  'fmax',
  'modf',
  'frexp',
  'ldexp',
  'scalbn',
  'scalbln',
  'ilogb',
  'copysign',
  'signbit',
  'isnan',
  'isinf',
  'isfinite',
  'isnormal',
  'fpclassify',
  'index',
  'rindex',
  'getenv',
  'setenv',
  'putenv',
  'system',
  'exit',
  'abort',
  'atoi',
  'atol',
  'atof',
  'atoll',
  'strtol',
  'strtoll',
  'strtoul',
  'strtoull',
  'strtod',
  'strtof',
  'strtold',
  'strchr',
  'strrchr',
  'strstr',
  'strspn',
  'strcspn',
  'strpbrk',
  'strtok',
  'strncpy',
  'strncat',
  'strcmp',
  'strncmp',
  'strcoll',
  'strxfrm',
  'strerror',
  'strlen',
  'memcpy',
  'memmove',
  'memcmp',
  'memchr',
  'memset',
  'malloc',
  'calloc',
  'realloc',
  'free',
  'getline',
  'getdelim',
  'open',
  'close',
  'read',
  'write',
  'lseek',
  'pipe',
  'fork',
  'wait',
  'kill',
  'sleep',
  'usleep',
  'alarm',
  'getpid',
  'getppid',
  'getuid',
  'geteuid',
  'getgid',
  'getegid',
  'getpgrp',
  'getsid',
  'setpgid',
  'setsid',
  'tcgetpgrp',
  'tcsetpgrp',
  'access',
  'link',
  'unlink',
  'rename',
  'remove',
  'mkdir',
  'rmdir',
  'chdir',
  'getcwd',
  'chmod',
  'chown',
  'stat',
  'fstat',
  'lstat',
  'umask',
  'sync',
  'sysconf',
  'pathconf',
  'fpathconf',
  'select',
  'poll',
  'signal',
  'raise',
  'longjmp',
  'setjmp',
  'qsort',
  'bsearch',
  'fopen',
  'fclose',
  'fread',
  'fwrite',
  'fseek',
  'ftell',
  'rewind',
  'fgetc',
  'fputc',
  'fgets',
  'fputs',
  'getc',
  'putc',
  'getchar',
  'putchar',
  'puts',
  'gets',
  'printf',
  'fprintf',
  'sprintf',
  'snprintf',
  'scanf',
  'fscanf',
  'sscanf'
])
