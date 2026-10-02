#include "expanders/tca9554/tca9554.h"
#include <array>
#include <cassert>
#include <cstdio>

class FakeBus final : public gea::chips::tca9554::Bus {
public:
  bool write(std::uint8_t reg, std::uint8_t value) override {
    if (fail) return false;
    registers.at(reg) = value;
    return true;
  }
  bool read(std::uint8_t reg, std::uint8_t &value) override {
    if (fail) return false;
    value = registers.at(reg);
    return true;
  }
  std::array<std::uint8_t, 4> registers{};
  bool fail = false;
};

int main() {
  FakeBus bus;
  gea::chips::tca9554::Driver driver;
  assert(driver.configure(bus, 0x27, 0x90));
  assert(bus.registers[1] == 0x27 && bus.registers[3] == 0x90);
  // Raising the amplifier preserves the panel's control signals.
  assert(driver.setPin(bus, 3, true));
  assert(bus.registers[1] == 0x2f);
  assert(driver.setInput(bus, 6, true));
  assert(bus.registers[3] == 0xd0);
  bus.fail = true;
  assert(!driver.setPin(bus, 5, false));
  assert(!driver.setInput(bus, 6, false));
  bus.fail = false;
  assert(driver.setPin(bus, 3, false));
  assert(bus.registers[1] == 0x27); // Failed reset write never poisoned the shadow.
  assert(driver.setInput(bus, 3, true));
  assert(bus.registers[3] == 0xd8);
  bus.registers[0] = 0x40;
  bool high = false;
  assert(driver.readPin(bus, 6, high) && high);
  assert(!driver.setPin(bus, 8, true));
  assert(!driver.setInput(bus, -1, true));
  puts("TCA9554 register and failure recovery tests passed");
}
