#pragma once

#include <Arduino.h>

#include <vector>

#include "runtime_config.h"

class NimBLEScan;
class NimBLEClient;

struct BleDeviceInfo {
  String name;
  String address;
  int rssi = 0;
};

struct BleStatus {
  bool initialized = false;
  bool scanning = false;
  bool connected = false;
  String deviceName;
  String deviceAddress;
  int rssi = 0;
  String lastError;
};

class BleManager {
 public:
  void begin();
  void configure(const RuntimeConfig &config);
  void tick();

  bool scanDevices(std::vector<BleDeviceInfo> &outDevices,
                   String *error = nullptr);
  bool connectToDevice(const String &address,
                       const String &name = "",
                       String *error = nullptr);
  void disconnectNow();

  bool isConnected() const;
  String lastError() const;
  BleStatus status() const;

 private:
  bool ensureInitialized(String *error = nullptr);
  void setError(const String &message);

  RuntimeConfig config_;
  NimBLEScan *scan_ = nullptr;
  NimBLEClient *client_ = nullptr;

  bool initialized_ = false;
  bool scanning_ = false;
  bool connected_ = false;

  String connectedName_;
  String connectedAddress_;
  int connectedRssi_ = 0;
  String lastError_;
};
