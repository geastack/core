// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"

#include <string>

namespace gea::host {

using GeolocationCoordinates =
    gea::framework::geolocation::GeolocationCoordinates;
using GeolocationPosition = gea::framework::geolocation::GeolocationPosition;
using GeolocationPositionError =
    gea::framework::geolocation::GeolocationPositionError;
using GeolocationOptions = gea::framework::geolocation::GeolocationOptions;

class AccelerometerValueProperty {
public:
  using Getter = double (*)();

  constexpr AccelerometerValueProperty() = default;
  explicit constexpr AccelerometerValueProperty(Getter getter)
      : getter_(getter) {}

  double operator()() const { return getter_ ? getter_() : 0.0; }

  operator double() const { return (*this)(); }

private:
  Getter getter_ = nullptr;
};

class AccelerometerFacade {
public:
  constexpr AccelerometerFacade()
      : tiltX(gea::framework::sensors::AccelerometerBackend::tiltX),
        tiltY(gea::framework::sensors::AccelerometerBackend::tiltY),
        x(gea::framework::sensors::AccelerometerBackend::accelerationX),
        y(gea::framework::sensors::AccelerometerBackend::accelerationY),
        z(gea::framework::sensors::AccelerometerBackend::accelerationZ),
        accelerationX(
            gea::framework::sensors::AccelerometerBackend::accelerationX),
        accelerationY(
            gea::framework::sensors::AccelerometerBackend::accelerationY),
        accelerationZ(
            gea::framework::sensors::AccelerometerBackend::accelerationZ),
        gyroscopeX(gea::framework::sensors::AccelerometerBackend::gyroscopeX),
        gyroscopeY(gea::framework::sensors::AccelerometerBackend::gyroscopeY),
        gyroscopeZ(gea::framework::sensors::AccelerometerBackend::gyroscopeZ) {}

  void start() const { gea::framework::sensors::AccelerometerBackend::init(); }
  void init() const { gea::framework::sensors::AccelerometerBackend::init(); }
  void close() const { gea::framework::sensors::AccelerometerBackend::close(); }
  void calibrateBias() const {
    gea::framework::sensors::AccelerometerBackend::calibrateBias();
  }

  AccelerometerValueProperty tiltX;
  AccelerometerValueProperty tiltY;
  AccelerometerValueProperty x;
  AccelerometerValueProperty y;
  AccelerometerValueProperty z;
  AccelerometerValueProperty accelerationX;
  AccelerometerValueProperty accelerationY;
  AccelerometerValueProperty accelerationZ;
  AccelerometerValueProperty gyroscopeX;
  AccelerometerValueProperty gyroscopeY;
  AccelerometerValueProperty gyroscopeZ;
};

using IMU = AccelerometerFacade;

inline constexpr AccelerometerFacade Accelerometer{};
inline constexpr const AccelerometerFacade &imu = Accelerometer;

class WiFiNetwork {
public:
  explicit WiFiNetwork(double index) : index_(index) {}
  std::string ssid() const {
    return gea::framework::network::WifiBackend::scanSsidAt(index_);
  }
  double rssi() const {
    return gea::framework::network::WifiBackend::scanRssiAt(index_);
  }
  bool secured() const {
    return gea::framework::network::WifiBackend::scanSecuredAt(index_);
  }

private:
  double index_ = 0.0;
};

class WiFi {
public:
  const WiFi &operator()() const { return *this; }
  bool enabled() const {
    return gea::framework::network::WifiBackend::enabled();
  }
  void setEnabled(bool enabled) const {
    gea::framework::network::WifiBackend::setEnabled(enabled);
  }
  bool connected() const {
    return gea::framework::network::WifiBackend::connected();
  }
  std::string ssid() const {
    return gea::framework::network::WifiBackend::ssid();
  }
  std::string ip() const { return gea::framework::network::WifiBackend::ip(); }
  std::string mac() const {
    return gea::framework::network::WifiBackend::mac();
  }
  double rssi() const { return gea::framework::network::WifiBackend::rssi(); }
  void configure(const std::string &ssid, const std::string &password) const {
    gea::framework::network::WifiBackend::configure(ssid, password);
  }
  bool waitForConnection(double timeoutMs) const {
    return gea::framework::network::WifiBackend::waitForConnection(timeoutMs);
  }
  void startScan() const { gea::framework::network::WifiBackend::startScan(); }
  bool scanning() const {
    return gea::framework::network::WifiBackend::scanning();
  }
  double scanCount() const {
    return gea::framework::network::WifiBackend::scanCount();
  }
  WiFiNetwork network(double index) const { return WiFiNetwork(index); }
  std::string scanSsidAt(double index) const { return network(index).ssid(); }
  double scanRssiAt(double index) const { return network(index).rssi(); }
  bool scanSecuredAt(double index) const { return network(index).secured(); }
};

class BluetoothKeyboard {
public:
  void tap(double hid_code) const {
    gea::framework::bluetooth::HidBackend::keyTap(hid_code);
  }
  void down(double modifier, double hid_code) const {
    gea::framework::bluetooth::HidBackend::keyDown(modifier, hid_code);
  }
  void up() const { gea::framework::bluetooth::HidBackend::keyUp(); }
};

class BluetoothMouse {
public:
  void move(double dx, double dy, double buttons = 0.0,
            double wheel = 0.0) const {
    gea::framework::bluetooth::HidBackend::mouseMove(dx, dy, buttons, wheel);
  }
  void click(double button) const {
    gea::framework::bluetooth::HidBackend::mouseClick(button);
  }
};

class BluetoothMidi {
public:
  // BLE-MIDI (MIDI over GATT). enable() registers the MIDI service with the
  // driver (idempotent; call before startAdvertising so the service UUID is
  // advertised). send() transmits one already-framed BLE-MIDI packet — the
  // TS side owns the header/timestamp framing.
  void enable() const { gea::framework::bluetooth::HidBackend::midiEnable(); }
  bool bound() const {
    return gea::framework::bluetooth::HidBackend::midiBound();
  }
  void send(const std::vector<double> &bytes) const {
    gea::framework::bluetooth::HidBackend::midiSend(bytes);
  }
  // Central role: scan for BLE-MIDI peripherals and connect as a GATT client.
  // Scan results use the WiFi-scan polling idiom (startScan / scanCount / *At).
  void startScan() const {
    gea::framework::bluetooth::HidBackend::midiStartScan();
  }
  void stopScan() const {
    gea::framework::bluetooth::HidBackend::midiStopScan();
  }
  bool scanning() const {
    return gea::framework::bluetooth::HidBackend::midiScanning();
  }
  double scanCount() const {
    return gea::framework::bluetooth::HidBackend::midiScanCount();
  }
  std::string scanNameAt(double index) const {
    return gea::framework::bluetooth::HidBackend::midiScanNameAt(index);
  }
  void connect(double index) const {
    gea::framework::bluetooth::HidBackend::midiConnect(index);
  }
  void disconnect() const {
    gea::framework::bluetooth::HidBackend::midiDisconnect();
  }
};

class BluetoothHidHost {
public:
  // HID host role: this device is the central and a remote HID peripheral
  // (keyboard/macro pad) is the input source. Scanning follows the same
  // polling idiom as MIDI; raw input reports queue driver-side until the
  // app drains them (reportCount / reportIdAt / reportLenAt / reportByteAt
  // / clearReports) and decodes the peripheral's report format itself.
  void startScan() const {
    gea::framework::bluetooth::HidBackend::hidHostStartScan();
  }
  void stopScan() const {
    gea::framework::bluetooth::HidBackend::hidHostStopScan();
  }
  bool scanning() const {
    return gea::framework::bluetooth::HidBackend::hidHostScanning();
  }
  double scanCount() const {
    return gea::framework::bluetooth::HidBackend::hidHostScanCount();
  }
  std::string scanNameAt(double index) const {
    return gea::framework::bluetooth::HidBackend::hidHostScanNameAt(index);
  }
  void connect(double index) const {
    gea::framework::bluetooth::HidBackend::hidHostConnect(index);
  }
  void disconnect() const {
    gea::framework::bluetooth::HidBackend::hidHostDisconnect();
  }
  bool bound() const {
    return gea::framework::bluetooth::HidBackend::hidHostBound();
  }
  double reportCount() const {
    return gea::framework::bluetooth::HidBackend::hidHostReportCount();
  }
  double reportIdAt(double index) const {
    return gea::framework::bluetooth::HidBackend::hidHostReportIdAt(index);
  }
  double reportLenAt(double index) const {
    return gea::framework::bluetooth::HidBackend::hidHostReportLenAt(index);
  }
  double reportByteAt(double index, double byteIndex) const {
    return gea::framework::bluetooth::HidBackend::hidHostReportByteAt(
        index, byteIndex);
  }
  void clearReports() const {
    gea::framework::bluetooth::HidBackend::hidHostClearReports();
  }
};

class BluetoothConnections {
public:
  // Live connection registry across every concurrent role. kind: 0 = HID
  // central peer (desktop using us as keyboard/mouse), 1 = MIDI central
  // peer (DAW), 2 = HID peripheral we host (macro pad), 3 = BLE-MIDI
  // peripheral we drive (pedal/WIDI adapter).
  double count() const {
    return gea::framework::bluetooth::HidBackend::connectionCount();
  }
  double kindAt(double index) const {
    return gea::framework::bluetooth::HidBackend::connectionKindAt(index);
  }
  std::string nameAt(double index) const {
    return gea::framework::bluetooth::HidBackend::connectionNameAt(index);
  }
};

class BluetoothConfig {
public:
  // Config service (custom GATT). A Web Bluetooth portal reads the app's
  // config blob and, once paired with the 4-digit code shown on-device,
  // writes a new one. setDocument() publishes the blob the portal reads
  // back (native std::vector<double>, like BluetoothMidi::send). The app
  // polls pendingLength() each frame; on a committed inbound blob it drains
  // pendingByteAt()/consumePending() and re-serializes via setDocument().
  // The pairing trio drives the on-device pairing overlay.
  void setDocument(const std::vector<double> &bytes) const {
    gea::framework::bluetooth::HidBackend::configSetDocument(bytes);
  }
  double pendingLength() const {
    return gea::framework::bluetooth::HidBackend::configPendingLength();
  }
  double pendingByteAt(double index) const {
    return gea::framework::bluetooth::HidBackend::configPendingByteAt(index);
  }
  void consumePending() const {
    gea::framework::bluetooth::HidBackend::configConsumePending();
  }
  bool pairing() const {
    return gea::framework::bluetooth::HidBackend::configPairing();
  }
  std::string pairCode() const {
    return gea::framework::bluetooth::HidBackend::configPairCode();
  }
  void dismissPairing() const {
    gea::framework::bluetooth::HidBackend::configDismissPairing();
  }
  // Device -> portal activity push (see BluetoothConfig::pushActivity in the TS
  // API).
  void pushActivity(double index) const {
    gea::framework::bluetooth::HidBackend::configPushActivity(index);
  }
};

class Bluetooth {
public:
  BluetoothKeyboard keyboard;
  BluetoothMouse mouse;
  BluetoothMidi midi;
  BluetoothHidHost hidHost;
  BluetoothConnections connections;
  BluetoothConfig config;

  const Bluetooth &operator()() const { return *this; }
  bool enabled() const {
    return gea::framework::bluetooth::HidBackend::enabled();
  }
  void setEnabled(bool enabled) const {
    gea::framework::bluetooth::HidBackend::setEnabled(enabled);
  }
  void init(const std::string &device_name, double appearance = 0.0,
            const std::string &mac_address = std::string()) const {
    gea::framework::bluetooth::HidBackend::init(device_name, appearance,
                                                mac_address);
  }
  void startAdvertising() const {
    gea::framework::bluetooth::HidBackend::startAdvertising();
  }
  void stopAdvertising() const {
    gea::framework::bluetooth::HidBackend::stopAdvertising();
  }
  bool connected() const {
    return gea::framework::bluetooth::HidBackend::connected();
  }
  bool bound() const { return gea::framework::bluetooth::HidBackend::bound(); }
  double batteryLevel() const {
    return gea::framework::bluetooth::HidBackend::batteryLevel();
  }
  std::string mac() const {
    return gea::framework::bluetooth::HidBackend::mac();
  }
  std::string deviceName() const {
    return gea::framework::bluetooth::HidBackend::deviceName();
  }
};

class Geolocation {
public:
  const Geolocation &operator()() const { return *this; }

  bool hasFix() const {
    return gea::framework::geolocation::GeolocationBackend::hasFix();
  }
  GeolocationPosition currentPosition() const {
    return gea::framework::geolocation::GeolocationBackend::currentPosition();
  }
  GeolocationCoordinates coords() const { return currentPosition().coords; }
  double latitude() const {
    return gea::framework::geolocation::GeolocationBackend::latitude();
  }
  double longitude() const {
    return gea::framework::geolocation::GeolocationBackend::longitude();
  }
  double accuracy() const {
    return gea::framework::geolocation::GeolocationBackend::accuracy();
  }

  template <typename Success> void getCurrentPosition(Success success) const {
    auto position = currentPosition();
    if (position.hasFix)
      success(position);
  }

  template <typename Success, typename Error>
  void getCurrentPosition(Success success, Error error) const {
    auto position = currentPosition();
    if (position.hasFix)
      success(position);
    else
      error(GeolocationPositionError{2.0, "Position unavailable"});
  }

  template <typename Success, typename Error, typename Options>
  void getCurrentPosition(Success success, Error error,
                          const Options &options) const {
    (void)options;
    getCurrentPosition(success, error);
  }

  template <typename Success> double watchPosition(Success success) const {
    getCurrentPosition(success);
    return 1.0;
  }

  template <typename Success, typename Error>
  double watchPosition(Success success, Error error) const {
    getCurrentPosition(success, error);
    return 1.0;
  }

  template <typename Success, typename Error, typename Options>
  double watchPosition(Success success, Error error,
                       const Options &options) const {
    (void)options;
    getCurrentPosition(success, error);
    return 1.0;
  }

  void clearWatch(double id) const { (void)id; }
};

class Navigator {
public:
  const char *userAgent = "gea-embedded";
  const char *language = "en-US";
  const char *platform = "embedded";
  bool onLine = true;
  WiFi wifi;
  Bluetooth bluetooth;
  Geolocation geolocation;
};

inline constexpr Navigator navigator{};

} // namespace gea::host
