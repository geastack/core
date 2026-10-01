import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const runtime = readFileSync(new URL('../runtime.cpp', import.meta.url), 'utf8')
const networkServices = readFileSync(new URL('../include/services/network_services.h', import.meta.url), 'utf8')

// Both radios need large contiguous internal allocations and the elastic-RAM
// arbiter is one-directional: the display yields staging to the radios and the
// radios never hand any back. So the boot order is load-bearing.
//
// BLE first: its controller needs one ~24-41 KB contiguous DRAM block and gets
// exactly one shot at it, before the app's mount and the framebuffers fragment
// internal RAM.
//
// Display before WiFi: bringing WiFi up first left the panel with a few hundred
// bytes of DMA-capable RAM and no flush staging -- a dark screen, while the
// framebuffer rendered correctly in PSRAM the whole time. Display::init() has to
// claim its floor first so WiFi bring-up shrinks it the designed way.
const ble = runtime.indexOf('services::BluetoothService::preinitForApp();')
assert.ok(ble >= 0, 'runtime should preinit the BLE controller for apps that use it')

// Anchored after the BLE preinit: an earlier Display::init() belongs to a
// different boot path.
const display = runtime.indexOf('gea::platform::display::Display::init()', ble)
const wifi = runtime.indexOf('network::wifi().init();', ble)
const reserve = runtime.indexOf('Display::reserveInternal(', display)
const applyReserve = runtime.indexOf('Display::applyPendingInternalReserve();', reserve)
assert.ok(display >= 0, 'runtime should initialize the display after the BLE preinit')
assert.ok(wifi >= 0, 'runtime should bring up early-connect WiFi')
assert.match(runtime.slice(reserve, applyReserve), /Display::flushBufferBytes\(\)/,
  'early startup must yield surplus staging for radio and application audio allocations')
assert.ok(
  display < reserve && reserve < applyReserve && applyReserve < wifi,
  'synchronous early WiFi must apply the display reservation before allocating the radio; ' +
    'the frame task cannot shrink staging until boot returns'
)

assert.ok(
  ble < display && display < wifi,
  'BLE must claim its one-shot controller allocation first, and the display must claim its ' +
    'DMA staging floor before WiFi bring-up shrinks it'
)

assert.equal(
  runtime.includes('AppManager::startLauncherButtonTask();'),
  false,
  'single-app firmware must not reserve a task for the retired resident-app launcher'
)
assert.match(networkServices, /bool diagnosticsEnabled = false;/)
