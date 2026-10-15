#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::chips::ft5336 {

inline constexpr std::uint8_t kI2cAddress = 0x38;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;
inline constexpr std::uint8_t kExpectedChipId = 0x51;
inline constexpr int kMaxTouchPoints = 5;

// id is the controller's touch ID: it stays with a finger while it is down.
struct MultiTouchSample {
	int count = 0;
	int id[kMaxTouchPoints] = {};
	int x[kMaxTouchPoints] = {};
	int y[kMaxTouchPoints] = {};
};

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegister(std::uint8_t reg, std::uint8_t value) = 0;
	virtual bool readRegister(std::uint8_t reg, std::uint8_t &value) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

// Coordinates are the controller's raw axes; the board maps them to the panel.
class ControllerCore {
public:
	bool configure(RegisterBus &bus);
	bool readChipId(RegisterBus &bus, std::uint8_t &chipId);
	bool readMulti(RegisterBus &bus, MultiTouchSample &sample);
};

}  // namespace gea::chips::ft5336
