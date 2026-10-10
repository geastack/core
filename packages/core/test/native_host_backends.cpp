// SPDX-License-Identifier: Apache-2.0
//
// The host BACKEND contract, faked for the native test harness.
//
// `host/backends.h` declares `HidBackend`, `WifiBackend` and
// `GeolocationBackend` as flat classes of static methods; the real
// implementations (`packages/host/host/ble.cpp`, `wifi.cpp`,
// `geolocation.cpp`) forward each one to a platform object -- `bluetooth()`,
// `wifi()`, `geolocation()` -- that this harness has no device for. So the
// harness fakes the backend layer itself, one no-op per declared method.
//
// It is linked UNCONDITIONALLY, unlike `native_test_host.cpp`, which the app
// pipeline tests exclude with `GEA_NATIVE_TEST_INCLUDE_HOST=0` so that a
// camera app can supply `CameraBackend` from its generated code. That
// exclusion used to be free: only a program that actually reached
// `navigator.bluetooth` referenced these symbols. It is not free any more --
// every gea app imports `@geastack/core`, whose runtime module wraps the WHOLE
// host surface, and the compiler emits those wrappers as reachable functions
// because they are stored in the module's exported object graph. So an app that
// never mentions bluetooth still links `HidBackend::midiSend`, and every
// pipeline test failed with "symbol(s) not found".
//
// Splitting the fakes out of `native_test_host.cpp` is what lets both kinds of
// test link the same one contract. The values are moved verbatim from there --
// `enabled() == true`, `deviceName() == "Gea Test HID"`, `ssid() == "Gea Test"`,
// `rssi() == -48` -- because `run-gea-hid-clicker-pipeline` asserts on them.
//
// Everything added beyond that answers "absent, empty, zero": a harness with no
// radio has no scan results, no bonded peers, and no pending config. A fake
// that invented some would make a test pass against a device that does not
// exist.

#include <string>
#include <vector>

#include "geolocation.h"
#include "host/backends.h"

namespace gea::framework::bluetooth {

void HidBackend::init(const std::string & /*device_name*/, double /*appearance*/, const std::string & /*mac_address*/) {}
bool HidBackend::enabled() { return true; }
void HidBackend::setEnabled(bool /*enabled*/) {}
void HidBackend::startAdvertising() {}
void HidBackend::stopAdvertising() {}
bool HidBackend::connected() { return false; }
bool HidBackend::bound() { return false; }
double HidBackend::batteryLevel() { return 100.0; }
std::string HidBackend::mac() { return "00:00:00:00:00:00"; }
std::string HidBackend::deviceName() { return "Gea Test HID"; }
void HidBackend::keyTap(double /*hid_code*/) {}
void HidBackend::keyDown(double /*modifier*/, double /*hid_code*/) {}
void HidBackend::keyUp() {}
void HidBackend::mouseMove(double /*dx*/, double /*dy*/, double /*buttons*/, double /*wheel*/) {}
void HidBackend::mouseClick(double /*button*/) {}

// MIDI over BLE: never enabled, so never bound and never scanning.
void HidBackend::midiEnable() {}
bool HidBackend::midiBound() { return false; }
void HidBackend::midiSend(const std::vector<double> & /*bytes*/) {}
void HidBackend::midiStartScan() {}
void HidBackend::midiStopScan() {}
bool HidBackend::midiScanning() { return false; }
double HidBackend::midiScanCount() { return 0.0; }
std::string HidBackend::midiScanNameAt(double /*index*/) { return ""; }
void HidBackend::midiConnect(double /*index*/) {}
void HidBackend::midiDisconnect() {}

// HID HOST (this device acting as the host for a peripheral keyboard/mouse):
// nothing to scan for, nothing bonded, no reports queued.
void HidBackend::hidHostStartScan() {}
void HidBackend::hidHostStopScan() {}
bool HidBackend::hidHostScanning() { return false; }
double HidBackend::hidHostScanCount() { return 0.0; }
std::string HidBackend::hidHostScanNameAt(double /*index*/) { return ""; }
void HidBackend::hidHostConnect(double /*index*/) {}
void HidBackend::hidHostDisconnect() {}
bool HidBackend::hidHostBound() { return false; }
double HidBackend::hidHostReportCount() { return 0.0; }
double HidBackend::hidHostReportIdAt(double /*index*/) { return 0.0; }
double HidBackend::hidHostReportLenAt(double /*index*/) { return 0.0; }
double HidBackend::hidHostReportByteAt(double /*index*/, double /*byteIndex*/) { return 0.0; }
void HidBackend::hidHostClearReports() {}

double HidBackend::connectionCount() { return 0.0; }
double HidBackend::connectionKindAt(double /*index*/) { return 0.0; }
std::string HidBackend::connectionNameAt(double /*index*/) { return ""; }

// The companion-app config channel: no peer, so nothing is ever pending and no
// pairing is ever in progress.
void HidBackend::configSetDocument(const std::vector<double> & /*bytes*/) {}
double HidBackend::configPendingLength() { return 0.0; }
double HidBackend::configPendingByteAt(double /*index*/) { return 0.0; }
void HidBackend::configConsumePending() {}
bool HidBackend::configPairing() { return false; }
std::string HidBackend::configPairCode() { return ""; }
void HidBackend::configDismissPairing() {}
void HidBackend::configPushActivity(double /*index*/) {}

}  // namespace gea::framework::bluetooth

namespace gea::framework::network {

bool WifiBackend::enabled() { return true; }
void WifiBackend::setEnabled(bool /*enabled*/) {}
bool WifiBackend::connected() { return true; }
double WifiBackend::rssi() { return -48.0; }
std::string WifiBackend::ssid() { return "Gea Test"; }
std::string WifiBackend::ip() { return "192.0.2.1"; }
std::string WifiBackend::mac() { return "00:00:00:00:00:00"; }
void WifiBackend::configure(const std::string & /*ssid*/, const std::string & /*password*/) {}
bool WifiBackend::startAccessPoint(const std::string & /*ssid*/, const std::string & /*password*/) { return false; }
void WifiBackend::stopAccessPoint() {}
std::string WifiBackend::accessPointMac() { return ""; }
bool WifiBackend::startCaptivePortal() { return false; }
void WifiBackend::stopCaptivePortal() {}
bool WifiBackend::waitForConnection(double /*timeoutMs*/) { return true; }
void WifiBackend::startScan() {}
bool WifiBackend::scanning() { return false; }
double WifiBackend::scanCount() { return 0.0; }
std::string WifiBackend::scanSsidAt(double /*index*/) { return ""; }
double WifiBackend::scanRssiAt(double /*index*/) { return 0.0; }
bool WifiBackend::scanSecuredAt(double /*index*/) { return false; }

}  // namespace gea::framework::network

namespace gea::framework::geolocation {

// No fix, so `currentPosition()` is the zero position rather than an invented
// coordinate. `latitude`/`longitude`/`accuracy` read that same position, which
// is what `geolocation.cpp` does with the real one.
bool GeolocationBackend::hasFix() { return false; }
GeolocationPosition GeolocationBackend::currentPosition() { return GeolocationPosition{}; }
double GeolocationBackend::latitude() { return currentPosition().coords.latitude; }
double GeolocationBackend::longitude() { return currentPosition().coords.longitude; }
double GeolocationBackend::accuracy() { return currentPosition().coords.accuracy; }

}  // namespace gea::framework::geolocation
