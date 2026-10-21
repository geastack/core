// The Gpio and ECMA-419 Digital classes over their one native backend, run
// against test_native_gpio_contract.cpp's fake pins.
import { Digital, Gpio } from '@geastack/core'

const led = new Gpio(2, { mode: 'output' })
led.write(true)
console.log('led ' + String(led.read()))

const key = new Gpio(4, {
  pull: 'up',
  onChange: (high: boolean) => {
    console.log('key ' + String(high))
    led.write(!high)
  },
})
console.log('key starts ' + String(key.read()))

let rises = 0
new Digital({
  pin: 5,
  mode: Digital.InputPullDown,
  edge: Digital.Rising,
  target: 'five',
  onReadable() {
    rises++
    console.log('readable ' + String(this.read()) + ' ' + String(this.target) + ' ' + String(rises))
  },
})

new Digital({
  pin: 6,
  mode: Digital.Input,
  edge: Digital.Rising | Digital.Falling,
  poll: 10,
  onReadable() {
    console.log('polled ' + String(this.read()))
  },
})

new Digital({
  pin: 8,
  mode: Digital.InputPullUp,
  edge: Digital.Falling,
  onReadable() {
    console.log('closing')
    this.close()
  },
})

try {
  new Gpio(4)
} catch {
  console.log('pin 4 is busy')
}
try {
  new Digital({ pin: 7, mode: Digital.Output, edge: Digital.Rising })
} catch {
  console.log('an output has no edge')
}

const out = new Digital({ pin: 9, mode: Digital.Output })
out.write(1)
console.log('out ' + String(out.read()) + ' ' + out.format)
console.log('legacy ' + String(Gpio.configureOutput(10)) + ' ' + String(Gpio.write(10, true)) + ' ' + String(Gpio.read(10)))
