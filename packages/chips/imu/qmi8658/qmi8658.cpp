#include "imu/qmi8658/qmi8658.h"

namespace gea::chips::qmi8658 {

namespace {

constexpr std::uint8_t kWhoAmI = 0x00;
constexpr std::uint8_t kCtrl1 = 0x02;
constexpr std::uint8_t kCtrl2 = 0x03;
constexpr std::uint8_t kCtrl3 = 0x04;
constexpr std::uint8_t kCtrl5 = 0x06;
constexpr std::uint8_t kCtrl7 = 0x08;
constexpr std::uint8_t kReset = 0x60;
constexpr std::uint8_t kTempLow = 0x33;
constexpr std::uint8_t kExpectedWhoAmI = 0x05;
constexpr int kBiasSamples = 100;
// Full scale must match resetAndConfigure(): CTRL2 0x15 selects +-4 g and CTRL3
// 0x54 selects +-512 dps.
constexpr double kAccelScale = 4.0 / 32768.0;
constexpr double kGyroScale = 512.0 / 32768.0;
constexpr double kStandardGravity = 9.80665;

std::int16_t readI16(const std::uint8_t *data, int offset) {
	return static_cast<std::int16_t>(data[offset] | (data[offset + 1] << 8));
}

}  // namespace

Driver::Driver(RegisterBus &bus, Delay &delay) : bus_(bus), delay_(delay) {}

bool Driver::begin() {
	if (open_) return true;

	std::uint8_t whoAmI = 0;
	if (!bus_.readRegister(kWhoAmI, whoAmI)) return false;
	lastWhoAmI_ = whoAmI;
	if (whoAmI != kExpectedWhoAmI) return false;

	open_ = resetAndConfigure();
	return open_;
}

void Driver::close() {
	open_ = false;
}

void Driver::calibrateBias() {
	if (!begin()) return;

	double sumGx = 0;
	double sumGy = 0;
	double sumGz = 0;
	int samples = 0;
	for (int i = 0; i < kBiasSamples; i++) {
		delay_.milliseconds(3);
		Sample sample{};
		if (!readSample(sample)) continue;
		sumGx += sample.gx;
		sumGy += sample.gy;
		sumGz += sample.gz;
		samples++;
	}

	if (samples == 0) return;
	biasX_ = sumGx / samples;
	biasY_ = sumGy / samples;
	biasZ_ = sumGz / samples;
}

bool Driver::readSample(Sample &sample) {
	std::uint8_t raw[14]{};
	if (!bus_.readRegisters(kTempLow, raw, sizeof(raw))) return false;

	const std::int16_t rax = readI16(raw, 2);
	const std::int16_t ray = readI16(raw, 4);
	const std::int16_t raz = readI16(raw, 6);
	const std::int16_t rgx = readI16(raw, 8);
	const std::int16_t rgy = readI16(raw, 10);
	const std::int16_t rgz = readI16(raw, 12);

	sample.ax = rax * kAccelScale;
	sample.ay = ray * kAccelScale;
	sample.az = raz * kAccelScale;
	sample.gx = rgx * kGyroScale;
	sample.gy = rgy * kGyroScale;
	sample.gz = rgz * kGyroScale;
	return true;
}

int Driver::tiltX() {
	Sample sample{};
	return readOpenSample(sample) ? tiltFromAxis(sample.ay) : 0;
}

int Driver::tiltY() {
	Sample sample{};
	return readOpenSample(sample) ? tiltFromAxis(-sample.ax) : 0;
}

double Driver::accelerationX() {
	Sample sample{};
	return readOpenSample(sample) ? sample.ax * kStandardGravity : 0.0;
}

double Driver::accelerationY() {
	Sample sample{};
	return readOpenSample(sample) ? sample.ay * kStandardGravity : 0.0;
}

double Driver::accelerationZ() {
	Sample sample{};
	return readOpenSample(sample) ? sample.az * kStandardGravity : 0.0;
}

double Driver::gyroscopeX() {
	Sample sample{};
	return readOpenSample(sample) ? sample.gx - biasX_ : 0.0;
}

double Driver::gyroscopeY() {
	Sample sample{};
	return readOpenSample(sample) ? sample.gy - biasY_ : 0.0;
}

double Driver::gyroscopeZ() {
	Sample sample{};
	return readOpenSample(sample) ? sample.gz - biasZ_ : 0.0;
}

std::uint8_t Driver::lastWhoAmI() const {
	return lastWhoAmI_;
}

bool Driver::readOpenSample(Sample &sample) {
	return begin() && readSample(sample);
}

bool Driver::resetAndConfigure() {
	if (!bus_.writeRegister(kReset, 0xB0)) return false;
	delay_.milliseconds(20);

	if (!bus_.writeRegister(kCtrl1, 0x40)) return false;
	if (!bus_.writeRegister(kCtrl2, 0x15)) return false;
	if (!bus_.writeRegister(kCtrl3, 0x54)) return false;
	if (!bus_.writeRegister(kCtrl5, 0x00)) return false;
	if (!bus_.writeRegister(kCtrl7, 0x03)) return false;
	delay_.milliseconds(50);
	return true;
}

int Driver::tiltFromAxis(double axis) {
	int value = static_cast<int>(axis * 70.0);
	if (value < -100) return -100;
	if (value > 100) return 100;
	return value;
}

}  // namespace gea::chips::qmi8658
