#pragma once

// BM8563 real-time clock. It is register-compatible with the NXP PCF8563:
// control/status 1 and 2 at 0x00/0x01, BCD time and date at 0x02-0x08 with the
// voltage-low flag in bit 7 of seconds and the century flag in bit 7 of
// months, and minute/hour/day/weekday alarms at 0x09-0x0c.

#include <cstddef>
#include <cstdint>

namespace gea::chips::bm8563 {

inline constexpr std::uint8_t kI2cAddress = 0x51;
inline constexpr std::uint32_t kI2cFrequencyHz = 400000;

struct DateTime {
	int year = 2000;  // 1900-2099; the century flag carries the high digit pair
	int month = 1;    // 1-12
	int day = 1;      // 1-31
	int weekday = 0;  // 0-6, 0 = Sunday
	int hour = 0;     // 0-23
	int minute = 0;   // 0-59
	int second = 0;   // 0-59
};

class RegisterBus {
public:
	virtual ~RegisterBus() = default;
	virtual bool writeRegisters(std::uint8_t reg, const std::uint8_t *data, std::size_t length) = 0;
	virtual bool readRegisters(std::uint8_t reg, std::uint8_t *data, std::size_t length) = 0;
};

class RealTimeClock {
public:
	explicit RealTimeClock(RegisterBus &bus) : bus_(bus) {}

	// Starts the oscillator (clears STOP and TEST) and disables the alarm and
	// timer interrupts, leaving the time registers as they are.
	bool begin();

	// `valid` is false when the voltage-low flag is set: the clock lost power
	// at some point and the time it holds cannot be trusted until it is set.
	bool readDateTime(DateTime &time, bool &valid);

	// Writes the time and date and clears the voltage-low flag.
	bool setDateTime(const DateTime &time);

	// Alarm on hour:minute (seconds are not alarmable on this part).
	bool setAlarm(int hour, int minute);
	bool disableAlarm();
	bool alarmFlag(bool &active);
	bool clearAlarmFlag();

	static std::uint8_t toBcd(int value);
	static int fromBcd(std::uint8_t value);

private:
	bool readRegister(std::uint8_t reg, std::uint8_t &value) { return bus_.readRegisters(reg, &value, 1); }
	bool writeRegister(std::uint8_t reg, std::uint8_t value) { return bus_.writeRegisters(reg, &value, 1); }

	RegisterBus &bus_;
};

}  // namespace gea::chips::bm8563
