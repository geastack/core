// SPDX-License-Identifier: Apache-2.0
#include "gpio.h"

#include "gea/embedded-host.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace gea::framework::gpio {

namespace {

// The TS surface hands every number over as a double. Truncating here — once,
// at the boundary — keeps the platform contract in ints and means a fractional
// pin number lands on a pin instead of on undefined behaviour.
int toPin(double value) { return static_cast<int>(value); }

}  // namespace

bool GpioBackend::configureOutput(double pin)
{
	return gea::platform::gpio::Gpio::configureOutput(toPin(pin));
}

bool GpioBackend::configureInput(double pin, bool pull_up)
{
	return gea::platform::gpio::Gpio::configureInput(toPin(pin), pull_up);
}

bool GpioBackend::write(double pin, bool level)
{
	return gea::platform::gpio::Gpio::write(toPin(pin), level);
}

bool GpioBackend::read(double pin) { return gea::platform::gpio::Gpio::read(toPin(pin)); }

namespace {

using Platform = gea::platform::gpio::Gpio;

// Open pins. A slot is also the pin's bit in Platform::takeEdges().
constexpr int kMaxOpen = 32;

struct OpenPin {
	bool used = false;
	int pin = -1;
	int mode = 0;
	int edges = 0;
	// Interrupt-driven, or polled every pollMs (0 = each frame).
	bool interrupt = false;
	int pollMs = 0;
	std::int64_t nextPollMs = 0;
	// The last level the app thread saw, for polled edge detection.
	bool level = false;
	std::function<void()> onEdge;
};

OpenPin gOpen[kMaxOpen];

std::int64_t nowMs()
{
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool isOutput(int mode) { return mode == Platform::Output || mode == Platform::OutputOpenDrain; }

OpenPin *lookup(double handle)
{
	const int slot = static_cast<int>(handle);
	if (slot < 0 || slot >= kMaxOpen || !gOpen[slot].used) return nullptr;
	return &gOpen[slot];
}

void deliverEdges()
{
	const unsigned fired = Platform::takeEdges();
	const std::int64_t now = nowMs();
	for (int slot = 0; slot < kMaxOpen; ++slot) {
		OpenPin &open = gOpen[slot];
		if (!open.used || !open.edges) continue;
		bool due = false;
		if (open.interrupt) {
			due = fired & (1u << slot);
			// An interrupt shared with another driver fires on both edges, and
			// a bouncing contact can end on either: a one-edge pin reports only
			// when it settled on that edge's level.
			if (due && open.edges != (Platform::kRising | Platform::kFalling))
				due = Platform::read(open.pin) == (open.edges == Platform::kRising);
		} else if (now >= open.nextPollMs) {
			open.nextPollMs = now + open.pollMs;
			const bool level = Platform::read(open.pin);
			if (level != open.level) {
				due = (open.edges & (level ? Platform::kRising : Platform::kFalling)) != 0;
				open.level = level;
			}
		}
		if (!due) continue;
		// The callback may close this pin (or open another into this slot);
		// run a copy so its captures outlive the call.
		auto onEdge = open.onEdge;
		if (onEdge) onEdge();
	}
}

}  // namespace

double GpioBackend::open(double pinValue, double modeValue, double edgesValue, double pollMsValue,
                         std::function<void()> onEdge)
{
	const int pin = toPin(pinValue);
	const int mode = toPin(modeValue);
	const int edges = toPin(edgesValue) & (Platform::kRising | Platform::kFalling);
	const int pollMs = toPin(pollMsValue);
	if (edges && isOutput(mode)) return -1;
	int slot = -1;
	for (int i = 0; i < kMaxOpen; ++i) {
		if (gOpen[i].used && gOpen[i].pin == pin) return -1;
		if (!gOpen[i].used && slot < 0) slot = i;
	}
	if (slot < 0 || !Platform::configure(pin, mode)) return -1;

	OpenPin &open = gOpen[slot];
	open = OpenPin{};
	open.used = true;
	open.pin = pin;
	open.mode = mode;
	open.edges = edges;
	open.level = isOutput(mode) ? false : Platform::read(pin);
	if (edges) {
		open.onEdge = std::move(onEdge);
		// Interrupts unless the app asked to poll; polling also stands in for
		// a platform or pin with no edge interrupt.
		open.interrupt = pollMs <= 0 && Platform::watch(pin, edges, slot);
		open.pollMs = pollMs > 0 ? pollMs : 0;
		gea::host::gpio::pendingCallbacks = deliverEdges;
	}
	return slot;
}

double GpioBackend::get(double handle)
{
	const OpenPin *open = lookup(handle);
	return open && Platform::read(open->pin) ? 1 : 0;
}

bool GpioBackend::set(double handle, double level)
{
	const OpenPin *open = lookup(handle);
	if (!open || !isOutput(open->mode)) return false;
	return Platform::write(open->pin, level != 0);
}

void GpioBackend::close(double handle)
{
	OpenPin *open = lookup(handle);
	if (!open) return;
	if (open->interrupt) Platform::unwatch(open->pin);
	// Destroying the callback releases its captures; when close() runs inside
	// that callback, deliverEdges holds the copy that is executing.
	auto released = std::move(open->onEdge);
	*open = OpenPin{};
}

void GpioBackend::runCallbacks() { deliverEdges(); }

bool AddressableLedBackend::set(double pin, double r, double g, double b)
{
	return gea::platform::gpio::AddressableLed::set(toPin(pin), toPin(r), toPin(g), toPin(b));
}

bool AddressableLedBackend::off(double pin) { return gea::platform::gpio::AddressableLed::off(toPin(pin)); }

bool AddressableLedBackend::attach(double pin, double count)
{
	return gea::platform::gpio::AddressableLed::attach(toPin(pin), toPin(count));
}

bool AddressableLedBackend::setPixel(double index, double r, double g, double b)
{
	return gea::platform::gpio::AddressableLed::setPixel(toPin(index), toPin(r), toPin(g), toPin(b));
}

bool AddressableLedBackend::show() { return gea::platform::gpio::AddressableLed::show(); }

void AddressableLedBackend::detach() { gea::platform::gpio::AddressableLed::detach(); }

}  // namespace gea::framework::gpio
