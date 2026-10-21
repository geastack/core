#include "power/axp2101/axp2101.h"

namespace gea::chips::axp2101 {

namespace {

// Register numbers and field layouts are the AXP2101's own, from its datasheet.
constexpr std::uint8_t kInputVoltageLimit = 0x15;
constexpr std::uint8_t kInputCurrentLimit = 0x16;
constexpr std::uint8_t kSystemPowerDownVoltage = 0x24;
constexpr std::uint8_t kAdcChannelControl = 0x30;
constexpr std::uint8_t kAdcVbusHigh = 0x38;
constexpr std::uint8_t kAdcVbusLow = 0x39;
constexpr std::uint8_t kAdcSystemHigh = 0x3A;
constexpr std::uint8_t kAdcSystemLow = 0x3B;
constexpr std::uint8_t kInterruptStatusFirst = 0x48;
constexpr std::uint8_t kInterruptStatusCount = 3;
constexpr std::uint8_t kInterruptEnable1 = 0x41;
constexpr std::uint8_t kInterruptStatus1 = 0x49;
// Bits of kInterruptEnable1 / kInterruptStatus1: the PWRON key's edges. The
// key pulls PWRON low, so the falling edge is the press.
constexpr std::uint8_t kPowerKeyRose = 0;
constexpr std::uint8_t kPowerKeyFell = 1;
constexpr std::uint8_t kPowerKeyEdges = (1u << kPowerKeyRose) | (1u << kPowerKeyFell);
constexpr std::uint8_t kChargeCurrentSetting = 0x62;
constexpr std::uint8_t kBatteryDetectControl = 0x68;
constexpr std::uint8_t kBatteryPercent = 0xA4;

constexpr std::uint8_t kBatteryDetectEnable = 0x01;

// Bits of kAdcChannelControl, one per measurement channel.
constexpr std::uint8_t kAdcBatteryVoltage = 0;
constexpr std::uint8_t kAdcBatteryTemperature = 1;
constexpr std::uint8_t kAdcVbusVoltage = 2;
constexpr std::uint8_t kAdcSystemVoltage = 3;

constexpr std::uint16_t kVbusVoltageLimitMin = 3880;
constexpr std::uint16_t kVbusVoltageLimitMax = 5080;
constexpr std::uint16_t kVbusVoltageLimitStep = 80;

constexpr std::uint16_t kSystemPowerDownMin = 2600;
constexpr std::uint16_t kSystemPowerDownMax = 3300;
constexpr std::uint16_t kSystemPowerDownStep = 100;

}  // namespace

PowerManagementUnit::PowerManagementUnit(RegisterBus &bus) : bus_(bus) {}

bool PowerManagementUnit::updateBits(std::uint8_t reg, std::uint8_t mask, std::uint8_t value) {
	std::uint8_t current = 0;
	if (!bus_.readRegister(reg, current)) return false;
	const std::uint8_t updated = static_cast<std::uint8_t>((current & ~mask) | (value & mask));
	return bus_.writeRegister(reg, updated);
}

bool PowerManagementUnit::setBit(std::uint8_t reg, std::uint8_t bit, bool enabled) {
	const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);
	return updateBits(reg, mask, enabled ? mask : 0);
}

// The ADC results are twelve bits across two registers, the high six in the
// first. Anything above those six bits in the high byte belongs to other fields.
int PowerManagementUnit::readMillivolts(std::uint8_t high, std::uint8_t low) {
	std::uint8_t hi = 0;
	std::uint8_t lo = 0;
	if (!bus_.readRegister(high, hi) || !bus_.readRegister(low, lo)) return -1;
	return static_cast<int>(((hi & 0x3F) << 8) | lo);
}

bool PowerManagementUnit::begin() {
	if (!enableDefaultAdcChannels()) return false;
	(void)enableBatteryDetection();
	return true;
}

// Read-modify-write rather than a blanket store: the thermistor channel shares
// this register, a board without a sensor turns it off, and that choice has to
// survive whatever order the board's own setup and this run in.
bool PowerManagementUnit::enableDefaultAdcChannels() {
	constexpr std::uint8_t channels = (1u << kAdcBatteryVoltage) | (1u << kAdcVbusVoltage) | (1u << kAdcSystemVoltage);
	return updateBits(kAdcChannelControl, channels, channels);
}

bool PowerManagementUnit::enableBatteryDetection() {
	return bus_.writeRegister(kBatteryDetectControl, kBatteryDetectEnable);
}

int PowerManagementUnit::batteryPercent() {
	std::uint8_t value = 0;
	if (!bus_.readRegister(kBatteryPercent, value)) return -1;

	int percent = value & 0x7F;
	return percent > 100 ? 100 : percent;
}

bool PowerManagementUnit::setVbusCurrentLimit(VbusCurrentLimit limit) {
	return updateBits(kInputCurrentLimit, 0x07, static_cast<std::uint8_t>(limit));
}

bool PowerManagementUnit::vbusCurrentLimit(VbusCurrentLimit &limit) {
	std::uint8_t value = 0;
	if (!bus_.readRegister(kInputCurrentLimit, value)) return false;
	limit = static_cast<VbusCurrentLimit>(value & 0x07);
	return true;
}

bool PowerManagementUnit::setVbusVoltageLimit(std::uint16_t millivolts) {
	if (millivolts < kVbusVoltageLimitMin || millivolts > kVbusVoltageLimitMax) return false;
	if ((millivolts - kVbusVoltageLimitMin) % kVbusVoltageLimitStep != 0) return false;
	const std::uint8_t step = static_cast<std::uint8_t>((millivolts - kVbusVoltageLimitMin) / kVbusVoltageLimitStep);
	return updateBits(kInputVoltageLimit, 0x0F, step);
}

bool PowerManagementUnit::setSystemPowerDownVoltage(std::uint16_t millivolts) {
	if (millivolts < kSystemPowerDownMin || millivolts > kSystemPowerDownMax) return false;
	if ((millivolts - kSystemPowerDownMin) % kSystemPowerDownStep != 0) return false;
	const std::uint8_t step = static_cast<std::uint8_t>((millivolts - kSystemPowerDownMin) / kSystemPowerDownStep);
	return updateBits(kSystemPowerDownVoltage, 0x07, step);
}

bool PowerManagementUnit::setChargeCurrent(ChargeCurrent current) {
	return updateBits(kChargeCurrentSetting, 0x1F, static_cast<std::uint8_t>(current));
}

bool PowerManagementUnit::setBatteryTemperatureMeasure(bool enabled) {
	return setBit(kAdcChannelControl, kAdcBatteryTemperature, enabled);
}

bool PowerManagementUnit::setVbusVoltageMeasure(bool enabled) {
	return setBit(kAdcChannelControl, kAdcVbusVoltage, enabled);
}

bool PowerManagementUnit::setSystemVoltageMeasure(bool enabled) {
	return setBit(kAdcChannelControl, kAdcSystemVoltage, enabled);
}

int PowerManagementUnit::vbusMillivolts() {
	return readMillivolts(kAdcVbusHigh, kAdcVbusLow);
}

int PowerManagementUnit::systemMillivolts() {
	return readMillivolts(kAdcSystemHigh, kAdcSystemLow);
}

// The status bits latch, and are cleared by writing them back.
bool PowerManagementUnit::clearInterrupts() {
	bool ok = true;
	for (std::uint8_t i = 0; i < kInterruptStatusCount; ++i) {
		ok = bus_.writeRegister(static_cast<std::uint8_t>(kInterruptStatusFirst + i), 0xFF) && ok;
	}
	return ok;
}

bool PowerManagementUnit::enablePowerKeyEdges() {
	// Drop any edge latched before the enable, then let the latches arm.
	return bus_.writeRegister(kInterruptStatus1, kPowerKeyEdges) &&
	       updateBits(kInterruptEnable1, kPowerKeyEdges, kPowerKeyEdges);
}

bool PowerManagementUnit::takePowerKeyEdges(bool &fell, bool &rose) {
	std::uint8_t status = 0;
	if (!bus_.readRegister(kInterruptStatus1, status)) return false;
	fell = (status >> kPowerKeyFell) & 1u;
	rose = (status >> kPowerKeyRose) & 1u;
	const std::uint8_t latched = status & kPowerKeyEdges;
	if (!latched) return true;
	return bus_.writeRegister(kInterruptStatus1, latched);
}

}  // namespace gea::chips::axp2101
