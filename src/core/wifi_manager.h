#pragma once

#include <Arduino.h>

#include <vector>

#include "runtime_config.h"

class WifiManager {
 public:
  void begin();
  void configure(const RuntimeConfig &config);
  void tick();
  void disconnect();

  bool isConnected() const;
  bool hasCredentials() const;
  String ssid() const;
  String ip() const;
  int rssi() const;

  bool scanNetworks(std::vector<String> &outSsids, String *error = nullptr);

 private:
  String targetSsid_;
  String targetPassword_;
  unsigned long lastConnectAttemptMs_ = 0;
};
