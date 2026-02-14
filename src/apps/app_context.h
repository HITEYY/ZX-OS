#pragma once

#include <Arduino.h>

#include "../core/runtime_config.h"

class WifiManager;
class GatewayClient;
class BleManager;
class TailscaleLiteClient;
class UIShell;

enum class AppId : uint8_t {
  OpenClaw = 0,
  Settings = 1,
  FileExplorer = 2,
  Tailscale = 3,
  AppMarket = 4,
};

struct AppContext {
  RuntimeConfig config;
  WifiManager *wifi = nullptr;
  GatewayClient *gateway = nullptr;
  BleManager *ble = nullptr;
  TailscaleLiteClient *tailscaleLite = nullptr;
  UIShell *ui = nullptr;
  bool configDirty = false;
};
