#pragma once

#include <Arduino.h>

enum class GatewayAuthMode : uint8_t {
  Token = 0,
  Password = 1,
};

struct RuntimeConfig {
  uint32_t version = 2;
  String wifiSsid;
  String wifiPassword;
  String gatewayUrl;
  GatewayAuthMode gatewayAuthMode = GatewayAuthMode::Token;
  String gatewayToken;
  String gatewayPassword;
  bool autoConnect = false;
  String bleDeviceName;
  String bleDeviceAddress;
  bool bleAutoConnect = false;
  String tailscaleLoginServer;
  String tailscaleAuthKey;
  String appMarketGithubRepo;
  String appMarketReleaseAsset;
  bool tailscaleLiteEnabled = false;
  String tailscaleLiteNodeIp;
  String tailscaleLitePrivateKey;
  String tailscaleLitePeerHost;
  uint16_t tailscaleLitePeerPort = 41641;
  String tailscaleLitePeerPublicKey;
};

enum class ConfigLoadSource : uint8_t {
  Defaults = 0,
  SdCard = 1,
  Nvs = 2,
};

RuntimeConfig makeDefaultConfig();

bool validateConfig(const RuntimeConfig &config, String *error = nullptr);
bool hasGatewayCredentials(const RuntimeConfig &config);

bool loadConfig(RuntimeConfig &outConfig,
                ConfigLoadSource *source = nullptr,
                bool *loadedFromNvs = nullptr,
                String *error = nullptr);
bool saveConfig(const RuntimeConfig &config, String *error = nullptr);
bool resetConfig(String *error = nullptr);

const char *gatewayAuthModeName(GatewayAuthMode mode);
