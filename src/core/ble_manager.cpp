#include "ble_manager.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <string>

#if __has_include(<NimBLEExtAdvertising.h>)
#define NIMBLE_V2_PLUS 1
#endif

namespace {

constexpr uint32_t kScanTimeMs = 5000;
constexpr uint32_t kScanTimeSec = 5;
constexpr uint16_t kScanInterval = 100;
constexpr uint16_t kScanWindow = 99;

bool containsAddress(const std::vector<BleDeviceInfo> &list,
                     const String &address) {
  for (std::vector<BleDeviceInfo>::const_iterator it = list.begin();
       it != list.end();
       ++it) {
    if (it->address.equalsIgnoreCase(address)) {
      return true;
    }
  }
  return false;
}

String safeDeviceName(const String &name, const String &fallbackAddress) {
  if (!name.isEmpty()) {
    return name;
  }
  return fallbackAddress;
}

}  // namespace

void BleManager::begin() {
  String error;
  ensureInitialized(&error);
}

void BleManager::configure(const RuntimeConfig &config) {
  const String prevSavedAddress = config_.bleDeviceAddress;
  config_ = config;

  if (connected_ && !prevSavedAddress.equalsIgnoreCase(config_.bleDeviceAddress) &&
      !connectedAddress_.equalsIgnoreCase(config_.bleDeviceAddress)) {
    disconnectNow();
  }
}

void BleManager::tick() {
  if (!client_) {
    return;
  }

  if (!client_->isConnected()) {
    if (connected_) {
      connected_ = false;
      connectedRssi_ = 0;
      if (lastError_.isEmpty()) {
        lastError_ = "BLE device disconnected";
      }
    }
    return;
  }

  connected_ = true;
  connectedRssi_ = client_->getRssi();
}

bool BleManager::scanDevices(std::vector<BleDeviceInfo> &outDevices,
                             String *error) {
  outDevices.clear();

  if (!ensureInitialized(error)) {
    return false;
  }

  if (!scan_) {
    if (error) {
      *error = "BLE scanner is unavailable";
    }
    setError("BLE scanner is unavailable");
    return false;
  }

  scanning_ = true;

  if (scan_->isScanning()) {
    scan_->stop();
  }

#ifdef NIMBLE_V2_PLUS
  NimBLEScanResults results = scan_->getResults(kScanTimeMs, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (!device) {
      continue;
    }

    const String address = String(device->getAddress().toString().c_str());
    if (address.isEmpty() || containsAddress(outDevices, address)) {
      continue;
    }

    BleDeviceInfo info;
    info.address = address;
    info.name = safeDeviceName(String(device->getName().c_str()), address);
    info.rssi = device->getRSSI();
    outDevices.push_back(info);
  }
#else
  NimBLEScanResults results = scan_->start(kScanTimeSec, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    NimBLEAdvertisedDevice device = results.getDevice(i);

    const String address = String(device.getAddress().toString().c_str());
    if (address.isEmpty() || containsAddress(outDevices, address)) {
      continue;
    }

    BleDeviceInfo info;
    info.address = address;
    info.name = safeDeviceName(String(device.getName().c_str()), address);
    info.rssi = device.getRSSI();
    outDevices.push_back(info);
  }
#endif

  std::sort(outDevices.begin(),
            outDevices.end(),
            [](const BleDeviceInfo &a, const BleDeviceInfo &b) {
              if (a.rssi == b.rssi) {
                return a.name < b.name;
              }
              return a.rssi > b.rssi;
            });

  scan_->clearResults();
  scanning_ = false;

  if (outDevices.empty()) {
    setError("No BLE devices found");
  } else {
    setError("");
  }

  return true;
}

bool BleManager::connectToDevice(const String &address,
                                 const String &name,
                                 String *error) {
  if (!ensureInitialized(error)) {
    return false;
  }

  if (address.isEmpty()) {
    if (error) {
      *error = "BLE address is empty";
    }
    setError("BLE address is empty");
    return false;
  }

  if (scan_ && scan_->isScanning()) {
    scan_->stop();
  }

  disconnectNow();

  NimBLEClient *nextClient = NimBLEDevice::createClient();
  if (!nextClient) {
    if (error) {
      *error = "Failed to allocate BLE client";
    }
    setError("Failed to allocate BLE client");
    return false;
  }

  nextClient->setConnectTimeout(5);

  const std::string addressStr(address.c_str());
  const NimBLEAddress publicAddress(addressStr, BLE_ADDR_PUBLIC);
  bool connected = nextClient->connect(publicAddress);

  if (!connected) {
    const NimBLEAddress randomAddress(addressStr, BLE_ADDR_RANDOM);
    connected = nextClient->connect(randomAddress);
  }

  if (!connected) {
    NimBLEDevice::deleteClient(nextClient);
    if (error) {
      *error = "BLE connect failed";
    }
    setError("BLE connect failed");
    return false;
  }

  client_ = nextClient;
  connected_ = true;
  connectedAddress_ = address;
  connectedName_ = name.isEmpty() ? connectedAddress_ : name;
  connectedRssi_ = client_->getRssi();
  setError("");
  return true;
}

void BleManager::disconnectNow() {
  if (client_) {
    if (client_->isConnected()) {
      client_->disconnect();
    }
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  connected_ = false;
  connectedRssi_ = 0;
  connectedName_ = "";
  connectedAddress_ = "";
}

bool BleManager::isConnected() const {
  return connected_;
}

String BleManager::lastError() const {
  return lastError_;
}

BleStatus BleManager::status() const {
  BleStatus state;
  state.initialized = initialized_;
  state.scanning = scanning_;
  state.connected = connected_;
  state.deviceName = connected_ ? connectedName_ : config_.bleDeviceName;
  state.deviceAddress = connected_ ? connectedAddress_ : config_.bleDeviceAddress;
  state.rssi = connectedRssi_;
  state.lastError = lastError_;
  return state;
}

bool BleManager::ensureInitialized(String *error) {
  if (initialized_) {
    return true;
  }

  NimBLEDevice::init("");
  scan_ = NimBLEDevice::getScan();
  if (!scan_) {
    if (error) {
      *error = "Failed to initialize BLE scanner";
    }
    setError("Failed to initialize BLE scanner");
    return false;
  }

  scan_->setActiveScan(true);
  scan_->setInterval(kScanInterval);
  scan_->setWindow(kScanWindow);

  initialized_ = true;
  return true;
}

void BleManager::setError(const String &message) {
  lastError_ = message;
}
