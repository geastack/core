#pragma once

// Digital pins, and a single-wire addressable LED strand.
//
// A board with no display still has to be able to say something. Until now the
// only outputs an app could reach were the panel and the log, which left a bare
// module — the exact hardware headless support was added for — with nothing it
// could drive. These two are the smallest primitives that fix that: a pin you
// can set, and the RGB LED that is the one output almost every devkit ships.
//
// Platforms without the hardware implement these as no-ops that report failure,
// so an app written against them still runs everywhere; it simply lights
// nothing. `ok` is therefore "this board did it", not "the call was valid".

namespace gea::platform::gpio {

class Gpio {
public:
	// Configure before use. Returns false when the pin is not a usable GPIO on
	// this chip, or when the platform has no GPIO at all.
	static bool configureOutput(int pin);
	static bool configureInput(int pin, bool pull_up);
	static bool write(int pin, bool level);
	// Reads low as false. A pin that was never configured, or a platform with no
	// GPIO, also reads false — callers that care must check configureInput().
	static bool read(int pin);

	// The ECMA-419 Digital modes, with the same numbers Moddable uses, so the
	// TS classes hand them straight through.
	enum Mode : int {
		Input = 0,
		InputPullUp = 1,
		InputPullDown = 2,
		InputPullUpDown = 3,
		Output = 8,
		OutputOpenDrain = 9,
	};
	// Edge masks for watch().
	static constexpr int kRising = 1;
	static constexpr int kFalling = 2;

	// configureInput/configureOutput for every mode above. False for a mode
	// this chip cannot do on this pin.
	static bool configure(int pin, int mode);

	// Interrupts on an input's edges (kRising | kFalling). Each edge sets bit
	// `slot` (0-31) of the mask takeEdges() returns and clears; nothing else
	// runs in interrupt context. False when this platform or pin has no edge
	// interrupt, and the caller then polls read() instead.
	static bool watch(int pin, int edges, int slot);
	static void unwatch(int pin);
	static unsigned takeEdges();
};

// WS2812-family LED on one data pin: the "onboard RGB LED" of most ESP32-S3
// devkits. Colours are staged by setPixel and only reach the strand on show(),
// because the protocol rewrites the whole strand in one timed burst.
class AddressableLed {
public:
	// The default way to turn one LED on or off: attaches the pin if it is not
	// already attached, writes the colour and clocks it out, in one call. A
	// board's "onboard LED" is a single WS2812 on one pin, and that case should
	// not cost three calls and a piece of retained state to express. Black
	// (0, 0, 0) is off.
	static bool set(int pin, int r, int g, int b);

	// Dark. The same thing as set(pin, 0, 0, 0) -- a WS2812 has no separate
	// "off", black IS off -- spelled as its own verb because that is what a
	// caller looks for, and because writing three zeros to mean "off" reads as
	// an accident at the call site. The channel stays attached, so turning the
	// LED back on costs one transmit and no re-allocation.
	static bool off(int pin);

	// The multi-pixel path, for a real strand where one transmit per FRAME beats
	// one per pixel: attach once, setPixel each element, show to clock the whole
	// buffer out. `set` is exactly these three for a strand of one.
	static bool attach(int pin, int count);
	static bool setPixel(int index, int r, int g, int b);
	static bool show();

	// Give the RMT channel back. The ESP32-S3 has four TX channels, so a strand
	// an app has finished with should not hold one, and the pin can then be
	// driven as a plain output again.
	static void detach();
};

}  // namespace gea::platform::gpio
