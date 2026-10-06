// SPDX-License-Identifier: Apache-2.0
#include "wifi.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>

EM_JS(int, gea_web_wifi_online, (), {
  return (typeof navigator !== 'undefined' && 'onLine' in navigator)
             ? (navigator.onLine ? 1 : 0)
             : 1;
});
#endif

namespace gea::framework::network {

namespace {

class NullWifiDriver final : public WifiDriver {
public:
  bool init() override { return false; }
  bool enabled() const override { return false; }
  void setEnabled(bool enabled) override { (void)enabled; }
  bool connected() const override { return false; }
  int rssi() override { return 0; }
  std::string ssid() override { return {}; }
  std::string ip() const override { return "0.0.0.0"; }
  std::string mac() override { return {}; }
  void configure(const std::string &ssid,
                 const std::string &password) override {
    (void)ssid;
    (void)password;
  }
  void scan() override {}
  bool scanning() const override { return false; }
  int scanCount() const override { return 0; }
  WifiNetwork networkAt(int index) const override {
    (void)index;
    return {};
  }
  std::vector<WifiNetwork> scanResults() const override { return {}; }
};

NullWifiDriver &nullDriver() {
  static NullWifiDriver driver;
  return driver;
}

#ifdef __EMSCRIPTEN__
// Browser builds (WASM simulator, web target): the network is ambient — the
// page either has connectivity or it doesn't, and fetch() rides the browser
// stack. Without this, the Null driver's connected()==false makes every app
// that gates fetches on WiFi.connected() (weather) sit on placeholders forever
// while the Emscripten fetch bridge would have worked fine. A web host can
// still install its own driver via WifiAdapter::setDriver (the DOM dev server
// shims do) — this is only the default.
class BrowserWifiDriver final : public WifiDriver {
public:
  bool init() override { return true; }
  bool enabled() const override { return enabled_; }
  void setEnabled(bool enabled) override { enabled_ = enabled; }
  bool connected() const override {
    return enabled_ && webConnected_ && gea_web_wifi_online() != 0;
  }
  int rssi() override { return rssi_; }
  std::string ssid() override { return ssid_; }
  std::string ip() const override { return ip_; }
  std::string mac() override { return {}; }
  void configure(const std::string &ssid,
                 const std::string &password) override {
    (void)password;
    ssid_ = ssid;
  }
  void scan() override {}
  bool scanning() const override { return false; }
  int scanCount() const override { return static_cast<int>(networks_.size()); }
  WifiNetwork networkAt(int index) const override {
    if (index < 0 || index >= static_cast<int>(networks_.size()))
      return {};
    return networks_[static_cast<std::size_t>(index)];
  }
  std::vector<WifiNetwork> scanResults() const override { return networks_; }
  // The simulator's Wi-Fi panel pushes its state through these hooks; honor
  // them so unchecking "Connected" actually takes apps offline in the sim.
  void setWebState(bool connected, const char *ssid, const char *ip,
                   int rssi) override {
    webConnected_ = connected;
    if (ssid)
      ssid_ = ssid;
    if (ip)
      ip_ = ip;
    rssi_ = rssi;
  }
  void setWebScanCount(int count) override {
    networks_.assign(count > 0 ? static_cast<std::size_t>(count) : 0,
                     WifiNetwork{});
  }
  void setWebScanEntry(int index, const char *ssid, int rssi,
                       bool secured) override {
    if (index < 0 || index >= static_cast<int>(networks_.size()))
      return;
    auto &entry = networks_[static_cast<std::size_t>(index)];
    entry.ssid = ssid ? ssid : "";
    entry.rssi = rssi;
    entry.secured = secured;
  }

private:
  bool enabled_ = true;
  bool webConnected_ = true;
  int rssi_ = -50;
  std::string ssid_ = "browser";
  std::string ip_ = "0.0.0.0";
  std::vector<WifiNetwork> networks_;
};

BrowserWifiDriver &browserDriver() {
  static BrowserWifiDriver driver;
  return driver;
}
#endif

WifiDriver *&driverSlot() {
#ifdef __EMSCRIPTEN__
  static WifiDriver *driver = &browserDriver();
#else
  static WifiDriver *driver = &nullDriver();
#endif
  return driver;
}

} // namespace

void WifiDriver::setWebState(bool connected, const char *ssid, const char *ip,
                             int rssi) {
  (void)connected;
  (void)ssid;
  (void)ip;
  (void)rssi;
}

void WifiDriver::setWebScanCount(int count) { (void)count; }

void WifiDriver::setWebScanEntry(int index, const char *ssid, int rssi,
                                 bool secured) {
  (void)index;
  (void)ssid;
  (void)rssi;
  (void)secured;
}

void WifiAdapter::setDriver(WifiDriver *driver) {
  driverSlot() = driver ? driver : &nullDriver();
}

WifiDriver *WifiAdapter::driver() { return driverSlot(); }

bool WifiAdapter::init() const { return driver()->init(); }
bool WifiAdapter::enabled() const { return driver()->enabled(); }
void WifiAdapter::setEnabled(bool enabled) const {
  driver()->setEnabled(enabled);
}
bool WifiAdapter::connected() const { return driver()->connected(); }
int WifiAdapter::rssi() const { return driver()->rssi(); }
std::string WifiAdapter::ssid() const { return driver()->ssid(); }
std::string WifiAdapter::ip() const { return driver()->ip(); }
std::string WifiAdapter::mac() const { return driver()->mac(); }
void WifiAdapter::configure(const std::string &ssid,
                            const std::string &password) const {
  driver()->configure(ssid, password);
}
void WifiAdapter::scan() const { driver()->scan(); }
bool WifiAdapter::scanning() const { return driver()->scanning(); }
int WifiAdapter::scanCount() const { return driver()->scanCount(); }
WifiNetwork WifiAdapter::networkAt(int index) const {
  return driver()->networkAt(index);
}
std::vector<WifiNetwork> WifiAdapter::scanResults() const {
  return driver()->scanResults();
}
void WifiAdapter::setWebState(bool connected, const char *ssid, const char *ip,
                              int rssi) const {
  driver()->setWebState(connected, ssid, ip, rssi);
}
void WifiAdapter::setWebScanCount(int count) const {
  driver()->setWebScanCount(count);
}
void WifiAdapter::setWebScanEntry(int index, const char *ssid, int rssi,
                                  bool secured) const {
  driver()->setWebScanEntry(index, ssid, rssi, secured);
}

} // namespace gea::framework::network
