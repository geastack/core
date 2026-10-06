// SPDX-License-Identifier: Apache-2.0
#include <string>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <chrono>
#include <thread>
#endif

#include "gea/embedded-host.h"
#include "wifi.h"

namespace gea::framework::network {

namespace {

void sleepForMs(int ms) {
#ifdef ESP_PLATFORM
  vTaskDelay(pdMS_TO_TICKS(ms));
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

} // namespace

bool WifiBackend::enabled() { return wifi().enabled(); }

void WifiBackend::setEnabled(bool enabled) { wifi().setEnabled(enabled); }

bool WifiBackend::connected() { return wifi().connected(); }

double WifiBackend::rssi() { return static_cast<double>(wifi().rssi()); }

std::string WifiBackend::ssid() { return wifi().ssid(); }

std::string WifiBackend::ip() { return wifi().ip(); }

std::string WifiBackend::mac() { return wifi().mac(); }

void WifiBackend::configure(const std::string &ssid,
                            const std::string &password) {
  wifi().configure(ssid, password);
}

bool WifiBackend::waitForConnection(double timeoutMs) {
  if (wifi().connected())
    return true;
  wifi().setEnabled(true);

  const int timeout = timeoutMs > 0 ? static_cast<int>(timeoutMs) : 0;
  int elapsed = 0;
  while (elapsed < timeout) {
    if (wifi().connected())
      return true;
    sleepForMs(100);
    elapsed += 100;
  }
  return wifi().connected();
}

void WifiBackend::startScan() { wifi().scan(); }

bool WifiBackend::scanning() { return wifi().scanning(); }

double WifiBackend::scanCount() {
  return static_cast<double>(wifi().scanCount());
}

std::string WifiBackend::scanSsidAt(double index) {
  return wifi().networkAt(static_cast<int>(index)).ssid;
}

double WifiBackend::scanRssiAt(double index) {
  return static_cast<double>(wifi().networkAt(static_cast<int>(index)).rssi);
}

bool WifiBackend::scanSecuredAt(double index) {
  return wifi().networkAt(static_cast<int>(index)).secured;
}

} // namespace gea::framework::network
