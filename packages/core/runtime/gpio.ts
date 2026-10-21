// Digital pins: two TS classes over one native backend.
//
// `Gpio` is the GeaStack class (string options, an `onChange(value)` callback,
// boolean levels). `Digital` is ECMA-419's Digital IO class (numeric modes, an
// `onReadable()` callback with `this` = the instance, levels 0 and 1), so code
// written for another ECMA-419 host runs unchanged. Both open the pin through
// `__gea_Gpio.open` (host/gpio.h, host/gpio.cpp): an input with edges is
// interrupt-driven, and its callback runs on the app thread at the start of
// the next frame. `poll: ms` opts a pin into polling instead; a platform or
// pin without an edge interrupt polls each frame on its own.

declare const __gea_Gpio: {
  configureOutput(pin: number): boolean
  configureInput(pin: number, pullUp: boolean): boolean
  write(pin: number, level: boolean): boolean
  read(pin: number): boolean
  open(pin: number, mode: number, edges: number, pollMs: number, onEdge: () => void): number
  get(handle: number): number
  set(handle: number, level: number): boolean
  close(handle: number): void
}

// gea::platform::gpio::Gpio::Mode, and the edge mask (Moddable's numbers).
const kInput = 0
const kInputPullUp = 1
const kInputPullDown = 2
const kInputPullUpDown = 3
const kOutput = 8
const kOutputOpenDrain = 9
const kRising = 1
const kFalling = 2

function isOutputMode(mode: number): boolean {
  return mode === kOutput || mode === kOutputOpenDrain
}

export type GpioMode = 'input' | 'output' | 'openDrain'
export type GpioPull = 'none' | 'up' | 'down' | 'both'
export type GpioEdge = 'none' | 'rising' | 'falling' | 'both'

export interface GpioOptions {
  /** Defaults to 'input'. */
  mode?: GpioMode
  /** An input's internal pull resistor. Defaults to 'none'. */
  pull?: GpioPull
  /** Which edges call `onChange`. Defaults to 'both' when `onChange` is given. */
  edge?: GpioEdge
  /** Called on the app thread with the level after an edge. */
  onChange?: (value: boolean) => void
  /** Poll every this many milliseconds instead of using the pin's interrupt. */
  poll?: number
}

function noChange(value: boolean): void {
  void value
}

export class Gpio {
  readonly pin: number
  private handle: number
  private edges: number
  private value: boolean
  private onChange: (value: boolean) => void

  constructor(pin: number, options: GpioOptions = {}) {
    this.pin = pin
    const kind = options.mode ?? 'input'
    let mode = kInput
    if (kind === 'output') mode = kOutput
    else if (kind === 'openDrain') mode = kOutputOpenDrain
    else if (options.pull === 'up') mode = kInputPullUp
    else if (options.pull === 'down') mode = kInputPullDown
    else if (options.pull === 'both') mode = kInputPullUpDown
    const edge = options.edge ?? (options.onChange !== undefined ? 'both' : 'none')
    let edges = 0
    if (edge === 'rising') edges = kRising
    else if (edge === 'falling') edges = kFalling
    else if (edge === 'both') edges = kRising | kFalling
    if (edges !== 0 && isOutputMode(mode)) throw new Error(`GPIO ${pin}: an output has no edges`)
    this.edges = edges
    this.value = false
    this.onChange = options.onChange ?? noChange
    this.handle = __gea_Gpio.open(pin, mode, edges, options.poll ?? 0, () => this.edge())
    if (this.handle < 0) throw new Error(`GPIO ${pin} cannot be opened as ${kind}`)
    if (edges !== 0) this.value = this.read()
  }

  /** The pin's level; for an output, the level it drives. */
  read(): boolean {
    if (this.handle < 0) throw new Error(`GPIO ${this.pin} is closed`)
    return __gea_Gpio.get(this.handle) !== 0
  }

  write(value: boolean): void {
    if (this.handle < 0) throw new Error(`GPIO ${this.pin} is closed`)
    if (!__gea_Gpio.set(this.handle, value ? 1 : 0)) throw new Error(`GPIO ${this.pin} is not an output`)
  }

  /** Releases the pin and its interrupt; the object cannot be used again. */
  close(): void {
    if (this.handle < 0) return
    __gea_Gpio.close(this.handle)
    this.handle = -1
  }

  private edge(): void {
    if (this.handle < 0) return
    const value = this.read()
    const changed = value !== this.value
    this.value = value
    // One edge filter reports every edge it was asked for (a rising-only pin
    // never sees the falls between); both edges report changes, which drops a
    // bounce that settled back to where it started.
    if (this.edges === kRising) {
      if (value) this.onChange(true)
    } else if (this.edges === kFalling) {
      if (!value) this.onChange(false)
    } else if (changed) {
      this.onChange(value)
    }
  }

  // The pin-number calls that predate the class. Each answers "did this board
  // do it": a pin the chip does not have reports false instead of throwing.
  static configureOutput(pin: number): boolean {
    return __gea_Gpio.configureOutput(pin)
  }

  static configureInput(pin: number, pullUp: boolean): boolean {
    return __gea_Gpio.configureInput(pin, pullUp)
  }

  static write(pin: number, level: boolean): boolean {
    return __gea_Gpio.write(pin, level)
  }

  static read(pin: number): boolean {
    return __gea_Gpio.read(pin)
  }
}

export interface DigitalOptions<Target = undefined> {
  pin: number
  /** Digital.Input, InputPullUp, InputPullDown, InputPullUpDown, Output or OutputOpenDrain. */
  mode: number
  /** Digital.Rising, Digital.Falling, or both OR'd; required with onReadable. */
  edge?: number
  /** Called with `this` = the Digital after an edge. */
  onReadable?: (this: Digital<Target>) => void
  /** Any value, kept as `target` for the callback. */
  target?: Target
  /** GeaStack extension: poll every this many milliseconds instead of using the interrupt. */
  poll?: number
  /** Only 'number' is supported. */
  format?: string
}

function notReadable<Target>(this: Digital<Target>): void {}

// ECMA-419 (2nd edition) Digital IO class.
export class Digital<Target = undefined> {
  static readonly Input = kInput
  static readonly InputPullUp = kInputPullUp
  static readonly InputPullDown = kInputPullDown
  static readonly InputPullUpDown = kInputPullUpDown
  static readonly Output = kOutput
  static readonly OutputOpenDrain = kOutputOpenDrain
  static readonly Rising = kRising
  static readonly Falling = kFalling

  readonly target: Target | undefined
  private handle: number
  private pin: number
  private onReadable: (this: Digital<Target>) => void

  constructor(options: DigitalOptions<Target>) {
    const edge = options.edge ?? 0
    if (options.format !== undefined && options.format !== 'number') throw new RangeError('Digital supports only format "number"')
    if (options.onReadable !== undefined && edge === 0) throw new RangeError('onReadable requires edge')
    if (edge !== 0 && isOutputMode(options.mode)) throw new RangeError('an output has no edge')
    this.pin = options.pin
    this.target = options.target
    this.onReadable = options.onReadable ?? notReadable
    this.handle = __gea_Gpio.open(options.pin, options.mode, edge, options.poll ?? 0, () => this.readable())
    if (this.handle < 0) throw new Error(`Digital: pin ${options.pin} cannot be opened in mode ${options.mode}`)
  }

  get format(): string {
    return 'number'
  }

  set format(value: string) {
    if (value !== 'number') throw new RangeError('Digital supports only format "number"')
  }

  /** 0 or 1. */
  read(): number {
    if (this.handle < 0) throw new Error(`Digital: pin ${this.pin} is closed`)
    return __gea_Gpio.get(this.handle)
  }

  write(value: number): void {
    if (this.handle < 0) throw new Error(`Digital: pin ${this.pin} is closed`)
    if (!__gea_Gpio.set(this.handle, value)) throw new Error(`Digital: pin ${this.pin} is not an output`)
  }

  close(): void {
    if (this.handle < 0) return
    __gea_Gpio.close(this.handle)
    this.handle = -1
  }

  private readable(): void {
    if (this.handle >= 0) this.onReadable()
  }
}
