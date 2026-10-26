#include "touch/ft5336/ft5336.h"

namespace gea::chips::ft5336 {

namespace {

constexpr std::uint8_t kTouchCountRegister = 0x02;
constexpr std::uint8_t kInterruptModeRegister = 0xA4;
constexpr std::uint8_t kInterruptModePolling = 0x00;
constexpr std::uint8_t kChipIdRegister = 0xA8;
constexpr std::size_t kPointStride = 6;

int coordinate(const std::uint8_t *high) {
	return ((high[0] & 0x0F) << 8) | high[1];
}

}  // namespace

bool ControllerCore::configure(RegisterBus &bus) {
	std::uint8_t chipId = 0;
	if (!readChipId(bus, chipId) || chipId != kExpectedChipId) return false;

	return bus.writeRegister(kInterruptModeRegister, kInterruptModePolling);
}

bool ControllerCore::readChipId(RegisterBus &bus, std::uint8_t &chipId) {
	return bus.readRegister(kChipIdRegister, chipId);
}

// Polled every frame, so the common case stays short: one burst for the count
// and the first point, a second only for extra fingers.
// A count above the controller's maximum is bus noise, not a value to clamp.
bool ControllerCore::readMulti(RegisterBus &bus, MultiTouchSample &sample) {
	std::uint8_t buf[1 + kMaxTouchPoints * kPointStride]{};
	sample.count = 0;
	if (!bus.readRegisters(kTouchCountRegister, buf, 1 + kPointStride)) return false;

	const int count = buf[0] & 0x0F;
	if (count == 0 || count > kMaxTouchPoints) return false;

	const std::size_t extraBytes = static_cast<std::size_t>(count - 1) * kPointStride;
	if (extraBytes && !bus.readRegisters(kTouchCountRegister + 1 + kPointStride, buf + 1 + kPointStride, extraBytes)) return false;

	for (int i = 0; i < count; ++i) {
		const std::uint8_t *point = buf + 1 + i * kPointStride;
		sample.id[i] = point[2] >> 4;
		sample.x[i] = coordinate(point);
		sample.y[i] = coordinate(point + 2);
	}
	sample.count = count;
	return true;
}

}  // namespace gea::chips::ft5336
