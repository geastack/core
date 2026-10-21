#pragma once

#include <cstdint>

namespace gea::chips::axp2101 {

inline constexpr std::uint8_t kI2cAddress = 0x34;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

// How much current the PMU will draw from VBUS. The default is low enough that
// a board powering anything downstream -- a USB host port feeding a bus-powered
// device -- browns out under it, which is why this is worth setting.
enum class VbusCurrentLimit : std::uint8_t {
	mA100 = 0,
	mA500,
	mA900,
	mA1000,
	mA1500,
	mA2000,
};

// Constant-charge current. The steps are the chip's own and are not evenly
// spaced, so they are named rather than computed.
enum class ChargeCurrent : std::uint8_t {
	mA0 = 0,
	mA100 = 4,
	mA125,
	mA150,
	mA175,
	mA200,
	mA300,
	mA400,
	mA500,
	mA600,
	mA700,
	mA800,
	mA900,
	mA1000,
};

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegister(std::uint8_t reg, std::uint8_t value) = 0;
	virtual bool readRegister(std::uint8_t reg, std::uint8_t &value) = 0;
};

class PowerManagementUnit {
public:
	explicit PowerManagementUnit(RegisterBus &bus);

	bool begin();

	// The measurement channels a board is read through: battery voltage, VBUS
	// and VSYS. The thermistor channel is deliberately not among them -- see
	// setBatteryTemperatureMeasure -- and whatever it is set to is left alone.
	bool enableDefaultAdcChannels();
	bool enableBatteryDetection();
	int batteryPercent();

	// What the PMU may take from VBUS, and how far VBUS may sag before the PMU
	// decides the supply is gone. A downstream device's inrush is a deep, brief
	// sag, so a board that hosts one wants the limit low rather than safe.
	bool setVbusCurrentLimit(VbusCurrentLimit limit);
	bool vbusCurrentLimit(VbusCurrentLimit &limit);
	// 3880 to 5080 mV, in steps of 80.
	bool setVbusVoltageLimit(std::uint16_t millivolts);

	// The VSYS the PMU shuts down at: 2600 to 3300 mV, in steps of 100.
	bool setSystemPowerDownVoltage(std::uint16_t millivolts);

	// Charging competes with everything else on the same supply, so a board fed
	// through one lead shared with a downstream device wants this small.
	bool setChargeCurrent(ChargeCurrent current);

	// The thermistor channel. A board with no battery temperature sensor has to
	// turn this off: left on, the charger reads an out-of-range temperature and
	// its own logic acts on it. It is off in the chip's reset state, and
	// enableDefaultAdcChannels does not turn it on.
	bool setBatteryTemperatureMeasure(bool enabled);
	bool setVbusVoltageMeasure(bool enabled);
	bool setSystemVoltageMeasure(bool enabled);

	// Millivolts, or -1 if the channel could not be read. Both read 0 until the
	// matching measurement channel is enabled.
	int vbusMillivolts();
	int systemMillivolts();

	bool clearInterrupts();

	// The power key's press and release, latched by the PMU as the PWRON
	// pin's falling and rising edges. A board with no PMU interrupt line reads
	// them from the status register.
	bool enablePowerKeyEdges();
	// Reports and clears the edges latched since the last call; both are set
	// when a whole press fit between two reads.
	bool takePowerKeyEdges(bool &fell, bool &rose);

private:
	bool updateBits(std::uint8_t reg, std::uint8_t mask, std::uint8_t value);
	bool setBit(std::uint8_t reg, std::uint8_t bit, bool enabled);
	int readMillivolts(std::uint8_t high, std::uint8_t low);

	RegisterBus &bus_;
};

}  // namespace gea::chips::axp2101
