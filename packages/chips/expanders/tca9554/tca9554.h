#pragma once

#include <cstdint>

namespace gea::chips::tca9554 {

class Bus {
public:
  virtual ~Bus() = default;
  virtual bool write(std::uint8_t reg, std::uint8_t value) = 0;
  virtual bool read(std::uint8_t reg, std::uint8_t &value) = 0;
};

// Register protocol shared by TCA9554/PCA9554. Configuration bits: 1=input.
class Driver {
public:
  bool configure(Bus &bus, std::uint8_t outputs, std::uint8_t directions) {
    if (!bus.write(1, outputs) || !bus.write(2, 0) || !bus.write(3, directions)) return false;
    outputs_ = outputs;
    directions_ = directions;
    return true;
  }

  bool setPin(Bus &bus, int pin, bool high) {
    if (pin < 0 || pin > 7) return false;
    const auto mask = static_cast<std::uint8_t>(1u << pin);
    const auto next = static_cast<std::uint8_t>(high ? outputs_ | mask : outputs_ & ~mask);
    if (!bus.write(1, next)) return false;
    outputs_ = next;
    return true;
  }

  bool setInput(Bus &bus, int pin, bool input) {
    if (pin < 0 || pin > 7) return false;
    const auto mask = static_cast<std::uint8_t>(1u << pin);
    const auto next = static_cast<std::uint8_t>(input ? directions_ | mask : directions_ & ~mask);
    if (!bus.write(3, next)) return false;
    directions_ = next;
    return true;
  }

  bool readPin(Bus &bus, int pin, bool &high) {
    if (pin < 0 || pin > 7) return false;
    std::uint8_t value = 0;
    if (!bus.read(0, value)) return false;
    high = (value & (1u << pin)) != 0;
    return true;
  }

private:
  std::uint8_t outputs_ = 255;
  std::uint8_t directions_ = 255;
};

}  // namespace gea::chips::tca9554
