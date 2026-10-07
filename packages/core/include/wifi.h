#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gea::framework::network {

struct WifiNetwork {
  std::string ssid;
  int rssi = 0;
  bool secured = false;
};

class WifiDriver {
public:
  virtual ~WifiDriver() = default;

  virtual bool init() = 0;
  virtual bool enabled() const = 0;
  virtual void setEnabled(bool enabled) = 0;
  virtual bool connected() const = 0;
  virtual int rssi() = 0;
  virtual std::string ssid() = 0;
  virtual std::string ip() const = 0;
  virtual std::string mac() = 0;
  virtual void configure(const std::string &ssid,
                         const std::string &password) = 0;
  virtual bool startAccessPoint(const std::string &, const std::string &) {
    return false;
  }
  virtual void stopAccessPoint() {}
  virtual std::string accessPointMac() { return {}; }
  virtual bool startCaptivePortal() { return false; }
  virtual void stopCaptivePortal() {}
  virtual void scan() = 0;
  virtual bool scanning() const = 0;
  virtual int scanCount() const = 0;
  virtual WifiNetwork networkAt(int index) const = 0;
  virtual std::vector<WifiNetwork> scanResults() const = 0;
  virtual void setWebState(bool connected, const char *ssid, const char *ip,
                           int rssi);
  virtual void setWebScanCount(int count);
  virtual void setWebScanEntry(int index, const char *ssid, int rssi,
                               bool secured);
};

class WifiAdapter {
public:
  static void setDriver(WifiDriver *driver);
  static WifiDriver *driver();

  bool init() const;
  bool enabled() const;
  void setEnabled(bool enabled) const;
  bool connected() const;
  int rssi() const;
  std::string ssid() const;
  std::string ip() const;
  std::string mac() const;
  void configure(const std::string &ssid, const std::string &password) const;
  bool startAccessPoint(const std::string &ssid,
                        const std::string &password) const;
  void stopAccessPoint() const;
  std::string accessPointMac() const;
  bool startCaptivePortal() const;
  void stopCaptivePortal() const;
  void scan() const;
  bool scanning() const;
  int scanCount() const;
  WifiNetwork networkAt(int index) const;
  std::vector<WifiNetwork> scanResults() const;
  void setWebState(bool connected, const char *ssid, const char *ip,
                   int rssi) const;
  void setWebScanCount(int count) const;
  void setWebScanEntry(int index, const char *ssid, int rssi,
                       bool secured) const;
};

inline WifiAdapter wifi() { return WifiAdapter{}; }

} // namespace gea::framework::network
