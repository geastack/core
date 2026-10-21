// native-gpio-contract.ts on fake pins: the platform half of include/gpio.h,
// with edges raised by setLevel() instead of an interrupt.
#include "gpio.h"
#include "host/gpio.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace gea::framework::graphics::generated { void ensureLinked() {} }
extern void __gea_top_level();

namespace {

using Platform = gea::platform::gpio::Gpio;

constexpr int kPins = 16;
bool gLevel[kPins];
int gMode[kPins];
int gWatchEdges[kPins];
int gWatchSlot[kPins];
unsigned gPending = 0;

bool valid(int pin) { return pin >= 0 && pin < kPins; }

void setLevel(int pin, bool level)
{
	if (gLevel[pin] == level) return;
	gLevel[pin] = level;
	if (gWatchEdges[pin] & (level ? Platform::kRising : Platform::kFalling)) gPending |= 1u << gWatchSlot[pin];
}

void frame(const char *step)
{
	std::printf("-- %s\n", step);
	std::fflush(stdout);
	gea::host::gpio::runCallbacks();
}

}  // namespace

namespace gea::platform::gpio {

bool Gpio::configureOutput(int pin) { return configure(pin, Output); }
bool Gpio::configureInput(int pin, bool pull_up) { return configure(pin, pull_up ? InputPullUp : InputPullDown); }

bool Gpio::configure(int pin, int mode)
{
	if (!valid(pin)) return false;
	gMode[pin] = mode;
	if (mode == InputPullUp) gLevel[pin] = true;
	else if (mode == InputPullDown || mode == Output) gLevel[pin] = false;
	return true;
}

bool Gpio::write(int pin, bool level)
{
	if (!valid(pin)) return false;
	gLevel[pin] = level;
	return true;
}

bool Gpio::read(int pin) { return valid(pin) && gLevel[pin]; }

bool Gpio::watch(int pin, int edges, int slot)
{
	if (!valid(pin)) return false;
	gWatchEdges[pin] = edges;
	gWatchSlot[pin] = slot;
	return true;
}

void Gpio::unwatch(int pin) { gWatchEdges[pin] = 0; }

unsigned Gpio::takeEdges()
{
	const unsigned edges = gPending;
	gPending = 0;
	return edges;
}

bool AddressableLed::set(int, int, int, int) { return false; }
bool AddressableLed::off(int) { return false; }
bool AddressableLed::attach(int, int) { return false; }
bool AddressableLed::setPixel(int, int, int, int) { return false; }
bool AddressableLed::show() { return false; }
void AddressableLed::detach() {}

}  // namespace gea::platform::gpio

int main()
{
	__gea_top_level();
	assert(gLevel[2] && gMode[2] == Platform::Output);
	assert(gMode[4] == Platform::InputPullUp && gWatchEdges[4] == (Platform::kRising | Platform::kFalling));
	assert(gMode[5] == Platform::InputPullDown && gWatchEdges[5] == Platform::kRising);
	// poll: 10 never takes the interrupt.
	assert(gWatchEdges[6] == 0);
	assert(gMode[9] == Platform::Output && gLevel[9]);

	setLevel(4, false);
	frame("key pressed");
	assert(gLevel[2]);
	setLevel(4, true);
	frame("key released");
	assert(!gLevel[2]);
	setLevel(4, false);
	setLevel(4, true);
	frame("bounce that settled back");

	setLevel(5, true);
	frame("pin 5 rises");
	setLevel(5, false);
	frame("pin 5 falls, not watched");
	setLevel(5, true);
	frame("pin 5 rises again");

	// Every frame so far polled pin 6 once and pushed its next poll 10 ms out.
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	setLevel(6, true);
	frame("polled pin rises");
	setLevel(6, false);
	frame("polled pin falls, before its next poll");
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	frame("polled pin is due");

	setLevel(8, false);
	frame("pin 8 falls");
	assert(gWatchEdges[8] == 0);
	setLevel(8, true);
	setLevel(8, false);
	frame("pin 8 is closed");
	std::printf("ok\n");
}
