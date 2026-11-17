#include "rtc/bm8563/bm8563.h"

namespace gea::chips::bm8563 {

namespace {

constexpr std::uint8_t kControl1 = 0x00;
constexpr std::uint8_t kControl2 = 0x01;
constexpr std::uint8_t kSeconds = 0x02;
constexpr std::uint8_t kMinuteAlarm = 0x09;

constexpr std::uint8_t kVoltageLow = 0x80;
constexpr std::uint8_t kCentury = 0x80;
constexpr std::uint8_t kAlarmDisabled = 0x80;
constexpr std::uint8_t kControl2AlarmFlag = 0x08;
constexpr std::uint8_t kControl2AlarmInterrupt = 0x02;

int clamp(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

}  // namespace

std::uint8_t RealTimeClock::toBcd(int value) {
	value = clamp(value, 0, 99);
	return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

int RealTimeClock::fromBcd(std::uint8_t value) {
	return ((value >> 4) & 0x0f) * 10 + (value & 0x0f);
}

bool RealTimeClock::begin() {
	return writeRegister(kControl1, 0x00) && writeRegister(kControl2, 0x00);
}

bool RealTimeClock::readDateTime(DateTime &time, bool &valid) {
	std::uint8_t data[7]{};
	if (!bus_.readRegisters(kSeconds, data, sizeof(data))) return false;
	valid = (data[0] & kVoltageLow) == 0;
	time.second = fromBcd(data[0] & 0x7f);
	time.minute = fromBcd(data[1] & 0x7f);
	time.hour = fromBcd(data[2] & 0x3f);
	time.day = fromBcd(data[3] & 0x3f);
	time.weekday = data[4] & 0x07;
	time.month = fromBcd(data[5] & 0x1f);
	time.year = fromBcd(data[6]) + ((data[5] & kCentury) ? 1900 : 2000);
	return true;
}

bool RealTimeClock::setDateTime(const DateTime &time) {
	const int year = clamp(time.year, 1900, 2099);
	const std::uint8_t data[7] = {
	    toBcd(clamp(time.second, 0, 59)),  // writing seconds clears the voltage-low flag
	    toBcd(clamp(time.minute, 0, 59)),
	    toBcd(clamp(time.hour, 0, 23)),
	    toBcd(clamp(time.day, 1, 31)),
	    static_cast<std::uint8_t>(clamp(time.weekday, 0, 6)),
	    static_cast<std::uint8_t>(toBcd(clamp(time.month, 1, 12)) | (year < 2000 ? kCentury : 0)),
	    toBcd(year % 100),
	};
	return bus_.writeRegisters(kSeconds, data, sizeof(data));
}

bool RealTimeClock::setAlarm(int hour, int minute) {
	const std::uint8_t data[4] = {toBcd(clamp(minute, 0, 59)), toBcd(clamp(hour, 0, 23)), kAlarmDisabled,
	                              kAlarmDisabled};
	if (!bus_.writeRegisters(kMinuteAlarm, data, sizeof(data))) return false;
	std::uint8_t control = 0;
	if (!readRegister(kControl2, control)) return false;
	control = static_cast<std::uint8_t>((control | kControl2AlarmInterrupt) & ~kControl2AlarmFlag);
	return writeRegister(kControl2, control);
}

bool RealTimeClock::disableAlarm() {
	const std::uint8_t data[4] = {kAlarmDisabled, kAlarmDisabled, kAlarmDisabled, kAlarmDisabled};
	if (!bus_.writeRegisters(kMinuteAlarm, data, sizeof(data))) return false;
	std::uint8_t control = 0;
	if (!readRegister(kControl2, control)) return false;
	return writeRegister(kControl2, static_cast<std::uint8_t>(control & ~(kControl2AlarmInterrupt | kControl2AlarmFlag)));
}

bool RealTimeClock::alarmFlag(bool &active) {
	std::uint8_t control = 0;
	if (!readRegister(kControl2, control)) return false;
	active = (control & kControl2AlarmFlag) != 0;
	return true;
}

bool RealTimeClock::clearAlarmFlag() {
	std::uint8_t control = 0;
	if (!readRegister(kControl2, control)) return false;
	return writeRegister(kControl2, static_cast<std::uint8_t>(control & ~kControl2AlarmFlag));
}

}  // namespace gea::chips::bm8563
