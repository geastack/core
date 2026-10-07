#pragma once

namespace gea::platform::power {

class Power {
public:
	static bool init();
	static int batteryPercent();
	static bool charging();
	static bool vibrate(double durationMs, int strength);
};

}  // namespace gea::platform::power
